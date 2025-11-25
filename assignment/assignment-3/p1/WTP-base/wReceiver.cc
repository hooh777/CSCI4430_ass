#include <iostream>
#include <fstream>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <map>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/stat.h>

#include "../starter_files/PacketHeader.h"
#include "../starter_files/crc32.h"

class WTPReceiver {
private:
    int sockfd;
    struct sockaddr_in my_addr;
    struct sockaddr_in sender_addr;
    socklen_t sender_addr_len;
    
    unsigned int window_size;
    std::string output_dir;
    std::ofstream log_file;
    
    int file_counter;  // For FILE-0.out, FILE-1.out, etc.
    
    // Connection state
    bool in_connection;
    unsigned int connection_seqNum;  // START/END seqNum for current connection
    unsigned int expected_seqNum;    // Next expected DATA seqNum
    
    // Receive buffer for out-of-order packets
    std::map<unsigned int, std::vector<char>> recv_buffer;
    
    // File data accumulator
    std::vector<char> file_data;
    
public:
    WTPReceiver(int port, unsigned int win_size, const char* out_dir, const char* log_path)
        : window_size(win_size), output_dir(out_dir), file_counter(0), 
          in_connection(false), connection_seqNum(0), expected_seqNum(0) {
        
        // Create UDP socket
        sockfd = socket(AF_INET, SOCK_DGRAM, 0);
        if (sockfd < 0) {
            perror("Socket creation failed");
            exit(1);
        }
        
        // Bind to port
        memset(&my_addr, 0, sizeof(my_addr));
        my_addr.sin_family = AF_INET;
        my_addr.sin_addr.s_addr = INADDR_ANY;
        my_addr.sin_port = htons(port);
        
        if (bind(sockfd, (struct sockaddr*)&my_addr, sizeof(my_addr)) < 0) {
            perror("Bind failed");
            exit(1);
        }
        
        sender_addr_len = sizeof(sender_addr);
        
        // Open log file
        log_file.open(log_path, std::ios::out | std::ios::trunc);
        if (!log_file.is_open()) {
            perror("Failed to open log file");
            exit(1);
        }
        
        // Ensure output directory exists
        mkdir(output_dir.c_str(), 0755);
    }
    
    ~WTPReceiver() {
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
    
    void send_ack(unsigned int seqNum) {
        PacketHeader ack;
        ack.type = PacketType::ACK;
        ack.seqNum = seqNum;
        ack.length = 0;
        ack.checksum = 0;
        
        sendto(sockfd, &ack, sizeof(ack), 0,
               (struct sockaddr*)&sender_addr, sender_addr_len);
        log_packet(ack);
    }
    
    void save_file() {
        std::string filename = output_dir + "/FILE-" + std::to_string(file_counter) + ".out";
        std::ofstream out_file(filename, std::ios::binary);
        
        if (out_file.is_open()) {
            out_file.write(file_data.data(), file_data.size());
            out_file.close();
        } else {
            std::cerr << "Failed to save file: " << filename << std::endl;
        }
        
        file_counter++;
    }
    
    void reset_connection() {
        in_connection = false;
        connection_seqNum = 0;
        expected_seqNum = 0;
        recv_buffer.clear();
        file_data.clear();
    }
    
    void handle_start(const PacketHeader& hdr) {
        if (in_connection) {
            // Already in a connection, ignore this START
            return;
        }
        
        // Start new connection
        in_connection = true;
        connection_seqNum = hdr.seqNum;
        expected_seqNum = 0;
        recv_buffer.clear();
        file_data.clear();
        
        // Send ACK with same seqNum
        send_ack(connection_seqNum);
    }
    
    void handle_end(const PacketHeader& hdr) {
        if (!in_connection) {
            return;
        }
        
        if (hdr.seqNum != connection_seqNum) {
            return;  // Wrong END seqNum
        }
        
        // Save the file
        save_file();
        
        // Send ACK
        send_ack(connection_seqNum);
        
        // Reset for next connection
        reset_connection();
    }
    
    void handle_data(const PacketHeader& hdr, const char* data) {
        if (!in_connection) {
            return;
        }
        
        // Verify checksum
        uint32_t calculated_checksum = crc32(data, hdr.length);
        if (calculated_checksum != hdr.checksum) {
            // Corrupted packet, drop silently
            return;
        }
        
        // Check if packet is within window
        if (hdr.seqNum >= expected_seqNum + window_size) {
            // Outside window, drop
            return;
        }
        
        if (hdr.seqNum < expected_seqNum) {
            // Already received this packet, send ACK for expected
            send_ack(expected_seqNum);
            return;
        }
        
        // Buffer the packet if not already buffered
        if (recv_buffer.find(hdr.seqNum) == recv_buffer.end()) {
            recv_buffer[hdr.seqNum] = std::vector<char>(data, data + hdr.length);
        }
        
        // Check if we can deliver packets in order
        while (recv_buffer.find(expected_seqNum) != recv_buffer.end()) {
            // Append to file data
            auto& pkt_data = recv_buffer[expected_seqNum];
            file_data.insert(file_data.end(), pkt_data.begin(), pkt_data.end());
            recv_buffer.erase(expected_seqNum);
            expected_seqNum++;
        }
        
        // Send cumulative ACK (next expected seqNum)
        send_ack(expected_seqNum);
    }
    
    void run() {
        char buffer[WTP_MTU + sizeof(PacketHeader)];
        
        while (true) {
            ssize_t received = recvfrom(sockfd, buffer, sizeof(buffer), 0,
                                        (struct sockaddr*)&sender_addr, &sender_addr_len);
            
            if (received < (ssize_t)sizeof(PacketHeader)) {
                continue;  // Invalid packet
            }
            
            PacketHeader hdr;
            memcpy(&hdr, buffer, sizeof(PacketHeader));
            log_packet(hdr);
            
            const char* data = buffer + sizeof(PacketHeader);
            
            switch (hdr.type) {
                case PacketType::START:
                    handle_start(hdr);
                    break;
                    
                case PacketType::END:
                    handle_end(hdr);
                    break;
                    
                case PacketType::DATA:
                    handle_data(hdr, data);
                    break;
                    
                case PacketType::ACK:
                    // Receiver shouldn't receive ACKs, ignore
                    break;
            }
        }
    }
};

int main(int argc, char* argv[]) {
    if (argc != 5) {
        std::cerr << "Usage: " << argv[0] << " <port-num> <window-size> <output-dir> <log>" << std::endl;
        return 1;
    }
    
    int port = atoi(argv[1]);
    unsigned int window_size = atoi(argv[2]);
    const char* output_dir = argv[3];
    const char* log_path = argv[4];
    
    WTPReceiver receiver(port, window_size, output_dir, log_path);
    receiver.run();
    
    return 0;
}
