#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <netinet/if_ether.h>

#include "sr_forward.h"
#include "sr_protocol.h"
#include "sr_rt.h"
#include "sr_arp.h"

void get_src_mac_by_iface(struct sr_instance *sr, const char* iface, uint8_t* mac) {
    for (int i = 0; i < SR_NUM_INTERFACES; i++) {
        if (strcmp(sr->ifaces[i].name, iface) == 0) {
            memcpy(mac, sr->ifaces[i].mac_addr, ETH_ALEN);
            return;
        }
    }
    memset(mac, 0, ETH_ALEN);
}

uint16_t ip_checksum(void* vdata, int length) {
    char* data = (char*)vdata;
    uint32_t sum = 0;
    uint16_t* ptr = (uint16_t*)data;
    
    for (; length > 1; length -= 2) {
        sum += *ptr++;
    }
    
    if (length == 1) {
        uint8_t left = 0;
        *((uint8_t*)&left) = *(uint8_t*)ptr;
        sum += left;
    }
    
    sum = (sum >> 16) + (sum & 0xffff);
    sum += (sum >> 16);
    
    return (uint16_t)~sum;
}

struct sr_rt* find_longest_prefix_match(struct sr_instance *sr, uint32_t dest_ip) {
    struct sr_rt* current = sr->routing_table;
    struct sr_rt* best_match = NULL;
    uint32_t longest_mask = 0;
    
    while (current != NULL) {
        uint32_t network_addr = current->dest.s_addr & current->mask.s_addr;
        uint32_t packet_network = dest_ip & current->mask.s_addr;
        
        if (network_addr == packet_network) {
            uint32_t mask_value = current->mask.s_addr;
            if (mask_value >= longest_mask) {
                longest_mask = mask_value;
                best_match = current;
            }
        }
        current = current->next;
    }
    
    return best_match;
}

struct sr_arpcache* find_arp_entry(struct sr_instance *sr, uint32_t ip) {
    struct sr_arpcache* current = sr->arp_cache;
    
    while (current != NULL) {
        if (current->ip.s_addr == ip) {
            return current;
        }
        current = current->next;
    }
    
    return NULL;
}

uint16_t debug_print_packet(uint8_t* buf, int len, const char* iface) {
    return 0;
}

const char* sr_handlepacket(struct sr_instance *sr, uint8_t* packet, int len, const char* recv_iface) {
    // Basic packet validation
    if (len < (int)sizeof(sr_ethernet_hdr_t)) {
        return NULL;
    }
    
    sr_ethernet_hdr_t* eth_hdr = (sr_ethernet_hdr_t*)packet;
    uint16_t eth_type = ntohs(eth_hdr->ether_type);
    
    // Only process IP packets
    if (eth_type != ethertype_ip) {
        return NULL;
    }
    
    // Verify packet has complete IP header
    if (len < (int)(sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t))) {
        return NULL;
    }
    
    sr_ip_hdr_t* ip_hdr = (sr_ip_hdr_t*)(packet + sizeof(sr_ethernet_hdr_t));
    
    // Verify IP header length
    if (ip_hdr->ip_hl < 5) {
        return NULL;
    }
    
    int ip_header_len = ip_hdr->ip_hl * 4;
    if (len < (int)(sizeof(sr_ethernet_hdr_t) + ip_header_len)) {
        return NULL;
    }
    
    // Verify IP checksum
    uint16_t saved_checksum = ip_hdr->ip_sum;
    ip_hdr->ip_sum = 0;
    uint16_t computed_checksum = ip_checksum(ip_hdr, ip_header_len);
    
    if (saved_checksum != computed_checksum) {
        ip_hdr->ip_sum = saved_checksum;
        return NULL;
    }
    ip_hdr->ip_sum = saved_checksum;
    
    // For TTL=1: set it to 0 then drop
    // For TTL=0: leave it as 0 and drop
    if (ip_hdr->ip_ttl == 1) {
        // Set TTL to 0 and drop the packet
        ip_hdr->ip_ttl = 0;
        
        // Update checksum since we modified TTL
        ip_hdr->ip_sum = 0;
        ip_hdr->ip_sum = ip_checksum(ip_hdr, ip_header_len);
        
        return NULL;
    }
    else if (ip_hdr->ip_ttl == 0) {
        // TTL is already 0, leave it unchanged and drop
        return NULL;
    }
    
    // Only process packets with TTL > 1
    // Decrement TTL
    ip_hdr->ip_ttl--;
    
    // Recompute IP checksum after TTL change
    ip_hdr->ip_sum = 0;
    ip_hdr->ip_sum = ip_checksum(ip_hdr, ip_header_len);
    
    // Find longest prefix match
    struct sr_rt* route = find_longest_prefix_match(sr, ip_hdr->ip_dst);
    if (route == NULL) {
        return NULL;
    }
    
    // Find ARP entry for next hop
    struct sr_arpcache* arp_entry = find_arp_entry(sr, route->gw.s_addr);
    if (arp_entry == NULL) {
        return NULL;
    }
    
    // Update Ethernet header
    memcpy(eth_hdr->ether_dhost, arp_entry->mac_addr, ETH_ALEN);
    
    uint8_t src_mac[ETH_ALEN];
    get_src_mac_by_iface(sr, route->interface, src_mac);
    memcpy(eth_hdr->ether_shost, src_mac, ETH_ALEN);
    
    eth_hdr->ether_type = htons(ethertype_ip);
    
    return route->interface;
}