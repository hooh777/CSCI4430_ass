#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <netinet/if_ether.h>
#include "sr_forward.h"
#include "sr_arp.h"

// Helper function to calculate IP checksum for sr_ip_hdr
uint16_t ip_checksum_sr(struct sr_ip_hdr* ip_hdr) {
    uint32_t sum = 0;
    int header_len = ip_hdr->ip_hl * 4; // IP header length in bytes
    uint16_t* data = (uint16_t*)ip_hdr;
    
    // Sum all 16-bit words
    for (int i = 0; i < header_len / 2; i++) {
        sum += ntohs(data[i]);
    }
    
    // Fold 32-bit sum to 16 bits
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    
    return htons(~sum);
}

// Helper function to find ARP cache entry
struct sr_arpcache* find_arp_entry(struct sr_instance *sr, uint32_t ip) {
    struct sr_arpcache* arp_walker = sr->arp_cache;
    
    while (arp_walker) {
        if (arp_walker->ip.s_addr == ip) {
            return arp_walker;
        }
        arp_walker = arp_walker->next;
    }
    return NULL;
}

// Get source MAC address for an interface
void get_src_mac_by_iface(struct sr_instance *sr, const char* iface, uint8_t* mac) {
    for (int i = 0; i < SR_NUM_INTERFACES; i++) {
        if (strcmp(sr->ifaces[i].name, iface) == 0) {
            memcpy(mac, sr->ifaces[i].mac_addr, ETH_ALEN);
            return;
        }
    }
    // If interface not found, use zeros (shouldn't happen in valid routing)
    memset(mac, 0, ETH_ALEN);
}

// Debug function to print packet information
uint16_t debug_print_packet(uint8_t* buf, int len, const char* iface) {
    if (len < sizeof(struct sr_ethernet_hdr)) {
        printf("[DEBUG] Packet too short on %s\n", iface);
        return 0;
    }
    
    struct sr_ethernet_hdr* eth_hdr = (struct sr_ethernet_hdr*)buf;
    
    printf("[DEBUG] Packet on %s: ", iface);
    printf("ETH %02x:%02x:%02x:%02x:%02x:%02x -> %02x:%02x:%02x:%02x:%02x:%02x ",
           eth_hdr->ether_shost[0], eth_hdr->ether_shost[1], eth_hdr->ether_shost[2],
           eth_hdr->ether_shost[3], eth_hdr->ether_shost[4], eth_hdr->ether_shost[5],
           eth_hdr->ether_dhost[0], eth_hdr->ether_dhost[1], eth_hdr->ether_dhost[2],
           eth_hdr->ether_dhost[3], eth_hdr->ether_dhost[4], eth_hdr->ether_dhost[5]);
    
    if (ntohs(eth_hdr->ether_type) == ethertype_ip) {
        printf("IP ");
        if (len >= sizeof(struct sr_ethernet_hdr) + sizeof(struct sr_ip_hdr)) {
            struct sr_ip_hdr* ip_hdr = (struct sr_ip_hdr*)(buf + sizeof(struct sr_ethernet_hdr));
            struct in_addr src_addr, dst_addr;
            src_addr.s_addr = ip_hdr->ip_src;
            dst_addr.s_addr = ip_hdr->ip_dst;
            char src_ip[INET_ADDRSTRLEN], dst_ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &src_addr, src_ip, INET_ADDRSTRLEN);
            inet_ntop(AF_INET, &dst_addr, dst_ip, INET_ADDRSTRLEN);
            printf("%s -> %s TTL:%d", src_ip, dst_ip, ip_hdr->ip_ttl);
        }
    } else if (ntohs(eth_hdr->ether_type) == ethertype_arp) {
        printf("ARP");
    } else {
        printf("Type:0x%04x", ntohs(eth_hdr->ether_type));
    }
    
    printf(" Len:%d\n", len);
    return ntohs(eth_hdr->ether_type);
}

// Main packet handling function
const char* sr_handlepacket(struct sr_instance *sr, uint8_t* packet, int len, const char* recv_iface) {
    // Step 1: Basic validation
    if (!packet || !sr || !recv_iface || len <= 0) {
        return NULL;
    }
    
    // Verify minimum Ethernet frame size
    if (len < sizeof(struct sr_ethernet_hdr)) {
        return NULL;
    }
    
    struct sr_ethernet_hdr* eth_hdr = (struct sr_ethernet_hdr*)packet;
    
    // Only handle IP packets
    if (ntohs(eth_hdr->ether_type) != ethertype_ip) {
        return NULL;
    }
    
    // Verify we have complete IP header
    if (len < sizeof(struct sr_ethernet_hdr) + sizeof(struct sr_ip_hdr)) {
        return NULL;
    }
    
    struct sr_ip_hdr* ip_hdr = (struct sr_ip_hdr*)(packet + sizeof(struct sr_ethernet_hdr));
    
    // Verify IP header length
    if (ip_hdr->ip_hl < 5) { // Minimum IP header length is 5 (20 bytes)
        return NULL;
    }
    
    uint16_t ip_header_len = ip_hdr->ip_hl * 4;
    if (ip_header_len < sizeof(struct sr_ip_hdr)) {
        return NULL;
    }
    
    // Verify packet length matches IP total length
    if (ntohs(ip_hdr->ip_len) > (len - sizeof(struct sr_ethernet_hdr))) {
        return NULL;
    }
    
    // Step 2: IP packet validation
    // Verify IP checksum
    uint16_t original_checksum = ip_hdr->ip_sum;
    
    // Create temporary copy for checksum calculation
    struct sr_ip_hdr temp_ip_hdr;
    memcpy(&temp_ip_hdr, ip_hdr, sizeof(struct sr_ip_hdr));
    temp_ip_hdr.ip_sum = 0;
    
    uint16_t calculated_checksum = ip_checksum_sr(&temp_ip_hdr);
    
    if (original_checksum != calculated_checksum) {
        return NULL; // Invalid checksum, drop packet
    }
    
    // Check TTL
    if (ip_hdr->ip_ttl <= 1) {
        return NULL; // TTL expired, drop packet
    }
    
    // Step 3: Routing table lookup
    if (!sr->routing_table) {
        return NULL; // No routing table
    }
    
    struct sr_rt* best_route = NULL;
    struct sr_rt* rt_walker = sr->routing_table;
    
    while (rt_walker) {
        if ((ip_hdr->ip_dst & rt_walker->mask.s_addr) == 
            (rt_walker->dest.s_addr & rt_walker->mask.s_addr)) {
            
            if (!best_route || 
                ntohl(rt_walker->mask.s_addr) > ntohl(best_route->mask.s_addr)) {
                best_route = rt_walker;
            }
        }
        rt_walker = rt_walker->next;
    }
    
    if (!best_route) {
        return NULL; // No route found
    }
    
    // Step 4: ARP cache lookup
    if (!sr->arp_cache) {
        return NULL; // No ARP cache
    }
    
    struct sr_arpcache* arp_entry = find_arp_entry(sr, best_route->gw.s_addr);
    if (!arp_entry) {
        return NULL; // ARP cache miss
    }
    
    // Step 5: Packet modification for forwarding
    // Decrement TTL
    ip_hdr->ip_ttl--;
    
    // Update IP checksum
    ip_hdr->ip_sum = 0;
    ip_hdr->ip_sum = ip_checksum_sr(ip_hdr);
    
    // Update Ethernet header
    memcpy(eth_hdr->ether_dhost, arp_entry->mac_addr, ETH_ALEN);
    
    // Get source MAC for outgoing interface
    uint8_t src_mac[ETH_ALEN];
    get_src_mac_by_iface(sr, best_route->interface, src_mac);
    memcpy(eth_hdr->ether_shost, src_mac, ETH_ALEN);
    
    return best_route->interface;
}