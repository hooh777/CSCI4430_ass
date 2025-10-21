#include "HTTPMessage.h"
#include "../common/network_utils.h"
#include "../common/LoadBalancerProtocol.h"

#include <cxxopts.hpp>
#include <spdlog/spdlog.h>
#include <pugixml.hpp>

#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

#include <string>
#include <map>
#include <vector>
#include <set>
#include <sstream>
#include <cstring>
#include <random>
#include <iostream>

// ============================================================================
// Data Structures
// ============================================================================

/**
 * Represents a connected client socket and its associated server connection
 */
struct ClientConnection {
    int client_fd;              // Socket to browser
    int server_fd;              // Socket to video server
    std::string server_ip;      // Video server IP
    int server_port;            // Video server port
    std::string client_ip;      // Client's IP address
    int client_port;            // Client's port
    
    std::string recv_buffer;    // Buffer for partial HTTP data from client
    std::string send_buffer;    // Buffer for partial HTTP data from server
    
    ClientConnection() : client_fd(-1), server_fd(-1), server_port(0), client_port(0) {}
};

/**
 * Tracks throughput for a specific client (identified by UUID)
 */
struct ClientThroughput {
    double avg_throughput_kbps;  // EWMA throughput estimate
    bool initialized;             // Whether we've received first segment
    
    ClientThroughput() : avg_throughput_kbps(0.0), initialized(false) {}
};

/**
 * Stores available bitrates for a video
 */
struct VideoInfo {
    std::vector<int> bitrates;   // Available bitrates in Kbps, sorted ascending
    std::string video_path;      // Path to video (e.g., /videos/tears-of-steel)
};

// ============================================================================
// Global State
// ============================================================================

// Configuration
bool use_load_balancer = false;
std::string lb_or_server_ip;
int lb_or_server_port;
double alpha_value;

// Connections
std::map<int, ClientConnection> connections;  // client_fd -> ClientConnection
std::map<int, int> server_to_client;          // server_fd -> client_fd

// Throughput tracking (per UUID)
std::map<std::string, ClientThroughput> client_throughputs;

// Video bitrate cache (path to .mpd -> VideoInfo)
std::map<std::string, VideoInfo> video_cache;

// ============================================================================
// Helper Functions
// ============================================================================

/**
 * Read exactly N bytes from socket (blocking)
 * Returns true on success, false on error/disconnect
 */
bool read_exact(int sockfd, char* buffer, size_t n) {
    size_t total = 0;
    while (total < n) {
        ssize_t result = recv(sockfd, buffer + total, n - total, 0);
        if (result <= 0) {
            return false;  // Error or disconnect
        }
        total += result;
    }
    return true;
}

/**
 * Read one byte at a time until we find "\r\n\r\n" (end of HTTP headers)
 * Returns the complete header string, or empty string on error
 */
std::string read_http_headers(int sockfd, std::string& leftover_buffer) {
    std::string headers = leftover_buffer;
    char byte;
    
    while (true) {
        ssize_t result = recv(sockfd, &byte, 1, 0);
        if (result <= 0) {
            return "";  // Error or disconnect
        }
        
        headers += byte;
        
        // Check if we've reached end of headers
        if (headers.size() >= 4) {
            size_t len = headers.size();
            if (headers[len-4] == '\r' && headers[len-3] == '\n' &&
                headers[len-2] == '\r' && headers[len-1] == '\n') {
                leftover_buffer.clear();
                return headers;
            }
        }
    }
}

/**
 * Parse HTTP headers into an HTTPMessage object
 * Returns true on success
 */
bool parse_http_message(const std::string& header_text, HTTPMessage& msg) {
    std::istringstream stream(header_text);
    std::string line;
    
    // Parse first line
    if (!std::getline(stream, line)) {
        return false;
    }
    // Remove \r if present
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    msg.parse_start_line(line);
    
    // Parse headers
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        
        // Empty line means end of headers
        if (line.empty()) {
            break;
        }
        
        // Split on first colon
        size_t colon_pos = line.find(':');
        if (colon_pos != std::string::npos) {
            std::string key = line.substr(0, colon_pos);
            std::string value = line.substr(colon_pos + 1);
            
            // Trim leading/trailing whitespace from value
            size_t start = value.find_first_not_of(" \t");
            size_t end = value.find_last_not_of(" \t");
            if (start != std::string::npos) {
                value = value.substr(start, end - start + 1);
            }
            
            msg.add_header(key, value);
        }
    }
    
    return true;
}

/**
 * Parse manifest file to extract available bitrates
 * Returns sorted vector of bitrates in Kbps
 */
std::vector<int> parse_manifest_bitrates(const std::string& manifest_content) {
    std::vector<int> bitrates;
    
    pugi::xml_document doc;
    pugi::xml_parse_result result = doc.load_string(manifest_content.c_str());
    
    if (!result) {
        spdlog::error("Failed to parse manifest XML");
        return bitrates;
    }
    
    // Find all Representation nodes in the video AdaptationSet
    // We want to skip audio representations
    for (pugi::xml_node adaptation_set : doc.select_nodes("//AdaptationSet").begin()->node().parent().children("AdaptationSet")) {
        std::string mime_type = adaptation_set.attribute("mimeType").value();
        
        // Only process video adaptations
        if (mime_type.find("video") != std::string::npos) {
            for (pugi::xml_node rep : adaptation_set.children("Representation")) {
                std::string bandwidth_str = rep.attribute("bandwidth").value();
                if (!bandwidth_str.empty()) {
                    try {
                        int bandwidth_kbps = std::stoi(bandwidth_str);
                        bitrates.push_back(bandwidth_kbps);
                    } catch (...) {
                        spdlog::warn("Failed to parse bandwidth: {}", bandwidth_str);
                    }
                }
            }
        }
    }
    
    // Sort bitrates in ascending order
    std::sort(bitrates.begin(), bitrates.end());
    
    return bitrates;
}

/**
 * Extract video path from a URI (directory containing the .mpd file)
 * E.g., "/videos/tears-of-steel/vid.mpd" -> "/videos/tears-of-steel"
 * E.g., "/videos/tears-of-steel/video/vid-500-seg-1.m4s" -> "/videos/tears-of-steel"
 */
std::string extract_video_path(const std::string& uri) {
    // Find the position of "/video/" which separates base path from video segments
    size_t video_dir_pos = uri.find("/video/");
    if (video_dir_pos != std::string::npos) {
        // This is a segment request, return everything before /video/
        return uri.substr(0, video_dir_pos);
    }
    
    // Otherwise, it's likely a manifest request - return directory containing the file
    size_t last_slash = uri.find_last_of('/');
    if (last_slash != std::string::npos) {
        return uri.substr(0, last_slash);
    }
    return uri;
}

/**
 * Check if URI is a video manifest request
 */
bool is_manifest_request(const std::string& uri) {
    return uri.find(".mpd") != std::string::npos;
}

/**
 * Check if URI is a video segment request
 * Must be .m4s AND in a /video/ directory (not audio)
 */
bool is_video_segment_request(const std::string& uri) {
    return uri.find(".m4s") != std::string::npos && 
           uri.find("/video/") != std::string::npos;
}

/**
 * Select appropriate bitrate based on current throughput
 * Rule: throughput >= 1.5 * bitrate
 */
int select_bitrate(const std::vector<int>& bitrates, double throughput_kbps) {
    if (bitrates.empty()) {
        return 0;
    }
    
    // Find highest bitrate that satisfies: throughput >= 1.5 * bitrate
    int selected = bitrates[0];  // Default to lowest
    
    for (int bitrate : bitrates) {
        if (throughput_kbps >= 1.5 * bitrate) {
            selected = bitrate;
        } else {
            break;  // Since sorted, no point checking higher bitrates
        }
    }
    
    return selected;
}

/**
 * Modify segment URI to use selected bitrate
 * E.g., "/videos/vid/video/vid-500-seg-2.m4s" -> "/videos/vid/video/vid-800-seg-2.m4s"
 */
std::string modify_segment_uri(const std::string& uri, int new_bitrate) {
    // Find "vid-XXX-seg" pattern
    size_t vid_pos = uri.find("vid-");
    if (vid_pos == std::string::npos) {
        return uri;  // Can't find pattern, return unchanged
    }
    
    size_t seg_pos = uri.find("-seg", vid_pos);
    if (seg_pos == std::string::npos) {
        return uri;
    }
    
    // Replace the bitrate part
    std::string result = uri.substr(0, vid_pos + 4);  // "...vid-"
    result += std::to_string(new_bitrate);
    result += uri.substr(seg_pos);  // "-seg-X.m4s"
    
    return result;
}

/**
 * Query load balancer for video server assignment
 * Returns true on success, fills in server_ip and server_port
 */
bool query_load_balancer(const std::string& client_ip, std::string& server_ip, int& server_port) {
    // Create socket to load balancer
    int lb_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (lb_fd < 0) {
        spdlog::error("Failed to create socket for load balancer");
        return false;
    }
    
    // Connect to load balancer
    struct sockaddr_in lb_addr;
    if (make_client_sockaddr(&lb_addr, lb_or_server_ip.c_str(), lb_or_server_port) < 0) {
        close(lb_fd);
        return false;
    }
    
    if (connect(lb_fd, (struct sockaddr*)&lb_addr, sizeof(lb_addr)) < 0) {
        spdlog::error("Failed to connect to load balancer");
        close(lb_fd);
        return false;
    }
    
    // Prepare request
    LoadBalancerRequest request;
    inet_pton(AF_INET, client_ip.c_str(), &request.client_addr);
    
    // Generate random request ID
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint16_t> dist(0, 65535);
    request.request_id = htons(dist(gen));
    
    // Send request
    if (send(lb_fd, &request, sizeof(request), 0) < 0) {
        spdlog::error("Failed to send request to load balancer");
        close(lb_fd);
        return false;
    }
    
    // Receive response
    LoadBalancerResponse response;
    if (recv(lb_fd, &response, sizeof(response), MSG_WAITALL) != sizeof(response)) {
        spdlog::error("Failed to receive response from load balancer");
        close(lb_fd);
        return false;
    }
    
    close(lb_fd);
    
    // Convert response to host order
    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &response.videoserver_addr, ip_str, INET_ADDRSTRLEN);
    server_ip = ip_str;
    server_port = ntohs(response.videoserver_port);
    
    return true;
}

/**
 * Create a simple HTTP 200 OK response
 */
std::string create_200_response() {
    return "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
}

// ============================================================================
// Main Proxy Logic
// ============================================================================

/**
 * Handle incoming client request
 */
void handle_client_request(ClientConnection& conn) {
    // Read HTTP headers
    std::string header_text = read_http_headers(conn.client_fd, conn.recv_buffer);
    if (header_text.empty()) {
        spdlog::info("Client socket sockfd {} disconnected", conn.client_fd);
        close(conn.client_fd);
        close(conn.server_fd);
        connections.erase(conn.client_fd);
        server_to_client.erase(conn.server_fd);
        return;
    }
    
    // Parse HTTP message
    HTTPMessage request;
    if (!parse_http_message(header_text, request)) {
        spdlog::error("Failed to parse HTTP request");
        return;
    }
    
    // Read body if present
    int content_length = request.get_content_length();
    if (content_length > 0) {
        request.body.resize(content_length);
        if (!read_exact(conn.client_fd, &request.body[0], content_length)) {
            spdlog::error("Failed to read request body");
            return;
        }
    }
    
    std::string client_uuid = request.get_header("x-489-uuid");
    
    // ========================================================================
    // Type C: POST /on-fragment-received - Update throughput
    // ========================================================================
    if (request.method == "POST" && request.uri == "/on-fragment-received") {
        // Extract timing information
        std::string size_str = request.get_header("x-fragment-size");
        std::string start_str = request.get_header("x-timestamp-start");
        std::string end_str = request.get_header("x-timestamp-end");
        
        if (!size_str.empty() && !start_str.empty() && !end_str.empty()) {
            try {
                long long segment_size = std::stoll(size_str);
                long long timestamp_start = std::stoll(start_str);
                long long timestamp_end = std::stoll(end_str);
                long long duration_ms = timestamp_end - timestamp_start;
                
                if (duration_ms > 0) {
                    // Calculate throughput for this segment (in Kbps)
                    double segment_throughput_kbps = (segment_size * 8.0) / duration_ms;  // bits per ms = Kbps
                    
                    // Update EWMA
                    auto& client_tput = client_throughputs[client_uuid];
                    if (!client_tput.initialized) {
                        client_tput.avg_throughput_kbps = segment_throughput_kbps;
                        client_tput.initialized = true;
                    } else {
                        client_tput.avg_throughput_kbps = 
                            alpha_value * segment_throughput_kbps + 
                            (1.0 - alpha_value) * client_tput.avg_throughput_kbps;
                    }
                    
                    spdlog::info("Client {} finished receiving a segment of size {} bytes in {} ms. "
                                "Throughput: {} Kbps. Avg Throughput: {} Kbps",
                                client_uuid, segment_size, duration_ms,
                                (int)segment_throughput_kbps, (int)client_tput.avg_throughput_kbps);
                }
            } catch (...) {
                spdlog::error("Failed to parse throughput info");
            }
        }
        
        // Send 200 OK response (don't forward to server)
        std::string response = create_200_response();
        send(conn.client_fd, response.c_str(), response.size(), 0);
        return;
    }
    
    // ========================================================================
    // Type A: Manifest file request
    // ========================================================================
    if (is_manifest_request(request.uri)) {
        std::string video_path = extract_video_path(request.uri);
        
        // Check if we've already cached this video's bitrates
        bool need_to_parse = (video_cache.find(video_path) == video_cache.end());
        
        if (need_to_parse) {
            // Request the regular manifest file to parse it
            HTTPMessage manifest_request = request;
            // Keep the original .mpd URI to fetch bitrate info
            std::string manifest_text = manifest_request.to_string();
            send(conn.server_fd, manifest_text.c_str(), manifest_text.size(), 0);
            
            // Read response
            std::string response_header = read_http_headers(conn.server_fd, conn.send_buffer);
            HTTPMessage manifest_response;
            parse_http_message(response_header, manifest_response);
            
            int manifest_content_length = manifest_response.get_content_length();
            if (manifest_content_length > 0) {
                manifest_response.body.resize(manifest_content_length);
                read_exact(conn.server_fd, &manifest_response.body[0], manifest_content_length);
            }
            
            // Parse bitrates
            VideoInfo info;
            info.video_path = video_path;
            info.bitrates = parse_manifest_bitrates(manifest_response.body);
            video_cache[video_path] = info;
            
            spdlog::debug("Parsed {} bitrates for {}", info.bitrates.size(), video_path);
        }
        
        // Now request the no-list manifest to send to client
        std::string no_list_uri = request.uri;
        size_t mpd_pos = no_list_uri.find(".mpd");
        if (mpd_pos != std::string::npos) {
            no_list_uri = no_list_uri.substr(0, mpd_pos) + "-no-list.mpd";
        }
        
        request.uri = no_list_uri;
        std::string modified_request = request.to_string();
        send(conn.server_fd, modified_request.c_str(), modified_request.size(), 0);
        
        // Read and forward response to client
        std::string response_header = read_http_headers(conn.server_fd, conn.send_buffer);
        send(conn.client_fd, response_header.c_str(), response_header.size(), 0);
        
        HTTPMessage response;
        parse_http_message(response_header, response);
        int response_content_length = response.get_content_length();
        if (response_content_length > 0) {
            char buffer[8192];
            int remaining = response_content_length;
            while (remaining > 0) {
                int to_read = std::min(remaining, (int)sizeof(buffer));
                int n = recv(conn.server_fd, buffer, to_read, 0);
                if (n <= 0) break;
                send(conn.client_fd, buffer, n, 0);
                remaining -= n;
            }
        }
        
        spdlog::info("Manifest requested by {} forwarded to {}:{} for {}",
                    client_uuid.empty() ? "unknown" : client_uuid, 
                    conn.server_ip, conn.server_port, no_list_uri);
        return;
    }
    
    // ========================================================================
    // Type B: Video segment request - Modify bitrate
    // ========================================================================
    if (is_video_segment_request(request.uri)) {
        std::string video_path = extract_video_path(request.uri);
        
        // Get available bitrates
        auto it = video_cache.find(video_path);
        if (it != video_cache.end() && !it->second.bitrates.empty()) {
            // Get current throughput for this client
            double current_throughput = 0.0;
            if (!client_uuid.empty()) {
                auto tput_it = client_throughputs.find(client_uuid);
                if (tput_it != client_throughputs.end()) {
                    current_throughput = tput_it->second.avg_throughput_kbps;
                }
            }
            
            // Select appropriate bitrate
            int selected_bitrate = select_bitrate(it->second.bitrates, current_throughput);
            
            // Modify URI
            std::string original_uri = request.uri;
            request.uri = modify_segment_uri(request.uri, selected_bitrate);
            
            // Forward modified request
            std::string modified_request = request.to_string();
            send(conn.server_fd, modified_request.c_str(), modified_request.size(), 0);
            
            // Read and forward response
            std::string response_header = read_http_headers(conn.server_fd, conn.send_buffer);
            send(conn.client_fd, response_header.c_str(), response_header.size(), 0);
            
            HTTPMessage response;
            parse_http_message(response_header, response);
            int response_content_length = response.get_content_length();
            if (response_content_length > 0) {
                char buffer[8192];
                int remaining = response_content_length;
                while (remaining > 0) {
                    int to_read = std::min(remaining, (int)sizeof(buffer));
                    int n = recv(conn.server_fd, buffer, to_read, 0);
                    if (n <= 0) break;
                    send(conn.client_fd, buffer, n, 0);
                    remaining -= n;
                }
            }
            
            spdlog::info("Segment requested by {} forwarded to {}:{} as {} at bitrate {} Kbps",
                        client_uuid.empty() ? "unknown" : client_uuid, 
                        conn.server_ip, conn.server_port, request.uri, selected_bitrate);
            return;
        } else {
            // No cached bitrates available - forward as-is
            spdlog::warn("No cached bitrates for video path: {}", video_path);
        }
    }
    
    // ========================================================================
    // Type D: All other requests - Forward as-is
    // ========================================================================
    std::string request_text = request.to_string();
    send(conn.server_fd, request_text.c_str(), request_text.size(), 0);
    
    // Read and forward response
    std::string response_header = read_http_headers(conn.server_fd, conn.send_buffer);
    send(conn.client_fd, response_header.c_str(), response_header.size(), 0);
    
    HTTPMessage response;
    parse_http_message(response_header, response);
    int response_content_length = response.get_content_length();
    if (response_content_length > 0) {
        char buffer[8192];
        int remaining = response_content_length;
        while (remaining > 0) {
            int to_read = std::min(remaining, (int)sizeof(buffer));
            int n = recv(conn.server_fd, buffer, to_read, 0);
            if (n <= 0) break;
            send(conn.client_fd, buffer, n, 0);
            remaining -= n;
        }
    }
}

/**
 * Accept new client connection and establish connection to video server
 */
void accept_new_client(int listen_fd) {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    
    int client_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &client_len);
    if (client_fd < 0) {
        spdlog::error("Failed to accept client connection");
        return;
    }
    
    // Get client info
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
    int client_port = ntohs(client_addr.sin_port);
    
    // Determine which video server to connect to
    std::string server_ip = lb_or_server_ip;
    int server_port = lb_or_server_port;
    
    if (use_load_balancer) {
        // Query load balancer
        if (!query_load_balancer(client_ip, server_ip, server_port)) {
            spdlog::error("Failed to get server from load balancer");
            close(client_fd);
            return;
        }
    }
    
    // Connect to video server
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        spdlog::error("Failed to create socket for video server");
        close(client_fd);
        return;
    }
    
    struct sockaddr_in server_addr;
    if (make_client_sockaddr(&server_addr, server_ip.c_str(), server_port) < 0) {
        close(client_fd);
        close(server_fd);
        return;
    }
    
    if (connect(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        spdlog::error("Failed to connect to video server {}:{}", server_ip, server_port);
        close(client_fd);
        close(server_fd);
        return;
    }
    
    // Create connection record
    ClientConnection conn;
    conn.client_fd = client_fd;
    conn.server_fd = server_fd;
    conn.server_ip = server_ip;
    conn.server_port = server_port;
    conn.client_ip = client_ip;
    conn.client_port = client_port;
    
    connections[client_fd] = conn;
    server_to_client[server_fd] = client_fd;
    
    spdlog::info("New client socket connected with {}:{} on sockfd {}", 
                client_ip, client_port, client_fd);
}

// ============================================================================
// Main Function
// ============================================================================

int main(int argc, char* argv[]) {
    // Parse command line arguments
    cxxopts::Options options("miProxy", "Adaptive bitrate HTTP proxy");
    options.add_options()
        ("l,listen-port", "Port to listen on", cxxopts::value<int>())
        ("h,hostname", "Hostname of server or load balancer", cxxopts::value<std::string>())
        ("p,port", "Port of server or load balancer", cxxopts::value<int>())
        ("a,alpha", "Alpha value for EWMA", cxxopts::value<double>())
        ("b,balance", "Use load balancing mode", cxxopts::value<bool>()->default_value("false"))
        ("help", "Print help");
    
    auto result = options.parse(argc, argv);
    
    if (result.count("help")) {
        std::cout << options.help() << std::endl;
        return 0;
    }
    
    // Validate arguments
    if (!result.count("listen-port") || !result.count("hostname") || 
        !result.count("port") || !result.count("alpha")) {
        spdlog::error("Missing required arguments");
        std::cout << options.help() << std::endl;
        return 1;
    }
    
    int listen_port = result["listen-port"].as<int>();
    lb_or_server_ip = result["hostname"].as<std::string>();
    lb_or_server_port = result["port"].as<int>();
    alpha_value = result["alpha"].as<double>();
    use_load_balancer = result["balance"].as<bool>();
    
    // Validate ranges
    if (listen_port < 1024 || listen_port > 65535) {
        spdlog::error("Listen port must be in range [1024, 65535]");
        return 1;
    }
    if (lb_or_server_port < 1024 || lb_or_server_port > 65535) {
        spdlog::error("Server/LB port must be in range [1024, 65535]");
        return 1;
    }
    if (alpha_value < 0.0 || alpha_value > 1.0) {
        spdlog::error("Alpha must be in range [0, 1]");
        return 1;
    }
    
    // Create listening socket
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        spdlog::error("Failed to create listening socket");
        return 1;
    }
    
    // Set SO_REUSEADDR to avoid "Address already in use" errors
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    // Bind
    struct sockaddr_in listen_addr;
    make_server_sockaddr(&listen_addr, listen_port);
    if (bind(listen_fd, (struct sockaddr*)&listen_addr, sizeof(listen_addr)) < 0) {
        spdlog::error("Failed to bind to port {}", listen_port);
        close(listen_fd);
        return 1;
    }
    
    // Listen
    if (listen(listen_fd, 50) < 0) {
        spdlog::error("Failed to listen");
        close(listen_fd);
        return 1;
    }
    
    spdlog::info("miProxy started");
    
    // Main event loop with select()
    while (true) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        
        // Add listening socket
        FD_SET(listen_fd, &read_fds);
        int max_fd = listen_fd;
        
        // Add all client sockets
        for (const auto& pair : connections) {
            FD_SET(pair.second.client_fd, &read_fds);
            if (pair.second.client_fd > max_fd) {
                max_fd = pair.second.client_fd;
            }
        }
        
        // Wait for activity
        int activity = select(max_fd + 1, &read_fds, nullptr, nullptr, nullptr);
        if (activity < 0) {
            spdlog::error("select() failed");
            break;
        }
        
        // Check for new connection
        if (FD_ISSET(listen_fd, &read_fds)) {
            accept_new_client(listen_fd);
        }
        
        // Check existing client connections
        // Make a copy of the map to avoid iterator invalidation
        std::vector<int> client_fds;
        for (const auto& pair : connections) {
            client_fds.push_back(pair.first);
        }
        
        for (int client_fd : client_fds) {
            // Check if this connection still exists (might have been closed)
            auto it = connections.find(client_fd);
            if (it == connections.end()) {
                continue;
            }
            
            if (FD_ISSET(client_fd, &read_fds)) {
                handle_client_request(it->second);
            }
        }
    }
    
    close(listen_fd);
    return 0;
}
