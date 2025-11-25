#include <iostream>
#include <fstream>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <chrono>
#include <vector>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

#include "../starter_files/PacketHeader.h"
#include "../starter_files/crc32.h"

class WTPSender {
private:
    int sockfd;
    struct sockaddr_in receiver_addr;
    socklen_t addr_len;
    
    unsigned int window_size;
    unsigned int start_seqNum;  // Random seqNum for START/END
    
    std::ofstream log_file;
    
    // Buffer for sending
    std::vector<std::vector<char>> packet_buffers;
    std::vector<size_t> packet_sizes;
    
public:
    WTPSender(const char* receiver_ip, int receiver_port, unsigned int win_size, const char* log_path) 
        : window_size(win_size) {
        
        // Create UDP socket
        sockfd = socket(AF_INET, SOCK_DGRAM, 0);
        if (sockfd < 0) {
            perror("Socket creation failed");
            exit(1);
        }
        
        // Set up receiver address
        memset(&receiver_addr, 0, sizeof(receiver_addr));
        receiver_addr.sin_family = AF_INET;
        receiver_addr.sin_port = htons(receiver_port);
        if (inet_pton(AF_INET, receiver_ip, &receiver_addr.sin_addr) <= 0) {
            perror("Invalid address");
            exit(1);
        }
        addr_len = sizeof(receiver_addr);
        
        // Open log file
        log_file.open(log_path, std::ios::out | std::ios::trunc);
        if (!log_file.is_open()) {
            perror("Failed to open log file");
            exit(1);
        }
        
        // Generate random start sequence number
        srand(time(nullptr));
        start_seqNum = rand() % 10000;
    }
    
    ~WTPSender() {
        if (log_file.is_open()) {
            log_file.close();
        }
        close(sockfd);
    }
    
    void log_packet(const PacketHeader& hdr) {
        log_file << static_cast<unsigned>(hdr.type) << " " 
                 << hdr.seqNum << " " 
                 << hdr.length << " " 
                 << hdr.checksum << std::endl;
    }
    
    bool send_packet(const PacketHeader& hdr, const char* data = nullptr) {
        size_t total_size = sizeof(PacketHeader) + hdr.length;
        std::vector<char> buffer(total_size);
        
        memcpy(buffer.data(), &hdr, sizeof(PacketHeader));
        if (data && hdr.length > 0) {
            memcpy(buffer.data() + sizeof(PacketHeader), data, hdr.length);
        }
        
        ssize_t sent = sendto(sockfd, buffer.data(), total_size, 0,
                              (struct sockaddr*)&receiver_addr, addr_len);
        
        if (sent > 0) {
            log_packet(hdr);
            return true;
        }
        return false;
    }
    
    bool receive_ack(PacketHeader& ack_hdr, int timeout_ms) {
        struct pollfd pfd;
        pfd.fd = sockfd;
        pfd.events = POLLIN;
        
        int ret = poll(&pfd, 1, timeout_ms);
        if (ret > 0 && (pfd.revents & POLLIN)) {
            char buffer[WTP_MTU];
            ssize_t received = recvfrom(sockfd, buffer, sizeof(buffer), 0, nullptr, nullptr);
            
            if (received >= (ssize_t)sizeof(PacketHeader)) {
                memcpy(&ack_hdr, buffer, sizeof(PacketHeader));
                log_packet(ack_hdr);
                return true;
            }
        }
        return false;
    }
    
    bool establish_connection() {
        PacketHeader start_pkt;
        start_pkt.type = PacketType::START;
        start_pkt.seqNum = start_seqNum;
        start_pkt.length = 0;
        start_pkt.checksum = 0;
        
        while (true) {
            send_packet(start_pkt);
            
            PacketHeader ack;
            if (receive_ack(ack, WTP_TIMEOUT.count())) {
                if (ack.type == PacketType::ACK && ack.seqNum == start_seqNum) {
                    return true;
                }
            }
            // Timeout - retry
        }
        return false;
    }
    
    bool terminate_connection() {
        PacketHeader end_pkt;
        end_pkt.type = PacketType::END;
        end_pkt.seqNum = start_seqNum;
        end_pkt.length = 0;
        end_pkt.checksum = 0;
        
        while (true) {
            send_packet(end_pkt);
            
            PacketHeader ack;
            if (receive_ack(ack, WTP_TIMEOUT.count())) {
                if (ack.type == PacketType::ACK && ack.seqNum == start_seqNum) {
                    return true;
                }
            }
            // Timeout - retry
        }
        return false;
    }
    
    void send_file(const char* input_file) {
        // Read entire file into memory
        std::ifstream file(input_file, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            std::cerr << "Failed to open input file: " << input_file << std::endl;
            exit(1);
        }
        
        std::streamsize file_size = file.tellg();
        file.seekg(0, std::ios::beg);
        
        std::vector<char> file_data(file_size);
        if (!file.read(file_data.data(), file_size)) {
            std::cerr << "Failed to read input file" << std::endl;
            exit(1);
        }
        file.close();
        
        // Split file into packets
        size_t offset = 0;
        unsigned int seqNum = 0;
        
        while (offset < (size_t)file_size) {
            size_t chunk_size = std::min((size_t)WTP_MDS, (size_t)file_size - offset);
            
            PacketHeader hdr;
            hdr.type = PacketType::DATA;
            hdr.seqNum = seqNum;
            hdr.length = chunk_size;
            hdr.checksum = crc32(file_data.data() + offset, chunk_size);
            
            std::vector<char> pkt_buffer(sizeof(PacketHeader) + chunk_size);
            memcpy(pkt_buffer.data(), &hdr, sizeof(PacketHeader));
            memcpy(pkt_buffer.data() + sizeof(PacketHeader), file_data.data() + offset, chunk_size);
            
            packet_buffers.push_back(std::move(pkt_buffer));
            packet_sizes.push_back(sizeof(PacketHeader) + chunk_size);
            
            offset += chunk_size;
            seqNum++;
        }
        
        // Sliding window transmission
        unsigned int total_packets = packet_buffers.size();
        unsigned int base = 0;  // First unacknowledged packet
        unsigned int next_seqNum = 0;  // Next packet to send
        
        while (base < total_packets) {
            // Send all packets in window
            while (next_seqNum < total_packets && next_seqNum < base + window_size) {
                ssize_t sent = sendto(sockfd, packet_buffers[next_seqNum].data(), 
                                      packet_sizes[next_seqNum], 0,
                                      (struct sockaddr*)&receiver_addr, addr_len);
                if (sent > 0) {
                    PacketHeader* hdr = (PacketHeader*)packet_buffers[next_seqNum].data();
                    log_packet(*hdr);
                }
                next_seqNum++;
            }
            
            // Wait for ACKs
            auto start_time = std::chrono::steady_clock::now();
            bool timeout = false;
            
            while (!timeout && base < total_packets) {
                auto elapsed = std::chrono::steady_clock::now() - start_time;
                auto remaining = WTP_TIMEOUT - std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
                
                if (remaining.count() <= 0) {
                    timeout = true;
                    break;
                }
                
                PacketHeader ack;
                if (receive_ack(ack, remaining.count())) {
                    if (ack.type == PacketType::ACK) {
                        // Cumulative ACK: ack.seqNum is the next expected seqNum
                        if (ack.seqNum > base) {
                            base = ack.seqNum;
                            start_time = std::chrono::steady_clock::now();  // Reset timer
                        }
                        
                        // Check if all packets in window are acknowledged
                        if (base >= next_seqNum) {
                            break;  // Move to send more packets
                        }
                    }
                }
            }
            
            // If timeout, retransmit from base
            if (timeout && base < total_packets) {
                next_seqNum = base;  // Reset next_seqNum to retransmit
            }
        }
    }
    
    void run(const char* input_file) {
        // Step 1: Establish connection
        if (!establish_connection()) {
            std::cerr << "Failed to establish connection" << std::endl;
            return;
        }
        
        // Step 2: Send file
        send_file(input_file);
        
        // Step 3: Terminate connection
        if (!terminate_connection()) {
            std::cerr << "Failed to terminate connection" << std::endl;
            return;
        }
    }
};

int main(int argc, char* argv[]) {
    if (argc != 6) {
        std::cerr << "Usage: " << argv[0] << " <receiver-IP> <receiver-port> <window-size> <input-file> <log>" << std::endl;
        return 1;
    }
    
    const char* receiver_ip = argv[1];
    int receiver_port = atoi(argv[2]);
    unsigned int window_size = atoi(argv[3]);
    const char* input_file = argv[4];
    const char* log_path = argv[5];
    
    WTPSender sender(receiver_ip, receiver_port, window_size, log_path);
    sender.run(input_file);
    
    return 0;
}
