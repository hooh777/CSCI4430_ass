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
#include <algorithm>

// ============================================================================
// Data Structures
// ============================================================================

struct ClientConnection {
    int client_fd;
    int server_fd;
    std::string server_ip;
    int server_port;
    std::string client_ip;
    int client_port;
    
    std::string recv_buffer;
    std::string send_buffer;
    
    ClientConnection() : client_fd(-1), server_fd(-1), server_port(0), client_port(0) {}
};

struct ClientThroughput {
    double avg_throughput_kbps;
    bool initialized;
    
    ClientThroughput() : avg_throughput_kbps(0.0), initialized(false) {}
};

struct VideoInfo {
    std::vector<int> bitrates;
    std::string video_path;
};

// ============================================================================
// Global State
// ============================================================================

bool use_load_balancer = false;
std::string lb_or_server_ip;
int lb_or_server_port;
double alpha_value;

std::map<int, ClientConnection> connections;
std::map<int, int> server_to_client;
std::map<std::string, ClientThroughput> client_throughputs;
std::map<std::string, VideoInfo> video_cache;

// ============================================================================
// Helper Functions
// ============================================================================

bool read_exact(int sockfd, char* buffer, size_t n) {
    size_t total = 0;
    while (total < n) {
        ssize_t result = recv(sockfd, buffer + total, n - total, 0);
        if (result <= 0) {
            return false;
        }
        total += result;
    }
    return true;
}

std::string read_http_headers(int sockfd, std::string& leftover_buffer) {
    std::string headers = leftover_buffer;
    char byte;
    
    while (headers.size() < 4 || 
           !(headers.size() >= 4 && 
             headers[headers.size()-4] == '\r' && 
             headers[headers.size()-3] == '\n' &&
             headers[headers.size()-2] == '\r' && 
             headers[headers.size()-1] == '\n')) {
        
        ssize_t result = recv(sockfd, &byte, 1, 0);
        if (result <= 0) {
            return "";
        }
        
        headers += byte;
    }
    
    leftover_buffer.clear();
    return headers;
}

bool parse_http_message(const std::string& header_text, HTTPMessage& msg) {
    std::istringstream stream(header_text);
    std::string line;
    
    // Parse first line
    if (!std::getline(stream, line)) {
        return false;
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    msg.parse_start_line(line);
    
    // Parse headers
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        
        if (line.empty()) {
            break;
        }
        
        size_t colon_pos = line.find(':');
        if (colon_pos != std::string::npos) {
            std::string key = line.substr(0, colon_pos);
            std::string value = line.substr(colon_pos + 1);
            
            // Trim whitespace from value
            size_t start = value.find_first_not_of(" \t");
            if (start != std::string::npos) {
                size_t end = value.find_last_not_of(" \t");
                value = value.substr(start, end - start + 1);
            } else {
                value = "";
            }
            
            msg.add_header(key, value);
        }
    }
    
    return true;
}

std::vector<int> parse_manifest_bitrates(const std::string& manifest_content) {
    std::vector<int> bitrates;
    
    pugi::xml_document doc;
    pugi::xml_parse_result result = doc.load_string(manifest_content.c_str());
    
    if (!result) {
        spdlog::error("Failed to parse manifest XML: {}", result.description());
        return bitrates;
    }
    
    // Try multiple approaches to find video bitrates
    auto representations = doc.select_nodes("//Representation");
    
    for (auto rep_node : representations) {
        pugi::xml_node rep = rep_node.node();
        std::string bandwidth_str = rep.attribute("bandwidth").value();
        
        if (!bandwidth_str.empty()) {
            try {
                int bandwidth = std::stoi(bandwidth_str);
                // The bandwidth in DASH manifests is typically in bps
                // Convert to Kbps for our calculations
                int bandwidth_kbps = bandwidth / 1000;
                
                // Only add valid positive bitrates
                if (bandwidth_kbps > 0) {
                    bitrates.push_back(bandwidth_kbps);
                    spdlog::debug("Found bitrate: {} bps -> {} Kbps", bandwidth, bandwidth_kbps);
                }
            } catch (const std::exception& e) {
                spdlog::warn("Failed to parse bandwidth '{}': {}", bandwidth_str, e.what());
            }
        }
    }
    
    // Remove duplicates and sort
    std::sort(bitrates.begin(), bitrates.end());
    bitrates.erase(std::unique(bitrates.begin(), bitrates.end()), bitrates.end());
    
    // FALLBACK: If no bitrates found, use default values
    if (bitrates.empty()) {
        spdlog::warn("No bitrates found in manifest, using defaults");
        bitrates = {500, 800, 1200, 2500, 5000}; // Common bitrates in Kbps
    }
    
    spdlog::info("Parsed {} unique bitrates: {}", bitrates.size(), fmt::join(bitrates, ", "));
    return bitrates;
}

std::string extract_video_key(const std::string& uri) {
    // Extract video identifier from path
    // "/videos/tears-of-steel/vid.mpd" -> "tears-of-steel"
    size_t videos_pos = uri.find("/videos/");
    if (videos_pos == std::string::npos) {
        return "";
    }
    
    size_t start = videos_pos + 8; // length of "/videos/"
    size_t end = uri.find("/", start);
    if (end == std::string::npos) {
        return uri.substr(start);
    }
    
    return uri.substr(start, end - start);
}

bool is_manifest_request(const std::string& uri) {
    return uri.find(".mpd") != std::string::npos;
}

bool is_video_segment_request(const std::string& uri) {
    return uri.find(".m4s") != std::string::npos && 
           uri.find("/video/") != std::string::npos;
}

int select_bitrate(const std::vector<int>& bitrates, double throughput_kbps) {
    if (bitrates.empty()) {
        spdlog::error("No bitrates available, cannot select bitrate");
        return 0;
    }
    
    // If throughput is 0 or very low, use lowest bitrate
    if (throughput_kbps <= 0) {
        return bitrates[0];
    }
    
    // Find highest bitrate that satisfies: throughput >= 1.5 * bitrate
    int selected = bitrates[0]; // Default to lowest
    
    for (int bitrate : bitrates) {
        if (throughput_kbps >= 1.5 * bitrate) {
            selected = bitrate;
        } else {
            break; // Since sorted, no point checking higher bitrates
        }
    }
    
    spdlog::debug("Selected bitrate {} Kbps for throughput {} Kbps", selected, throughput_kbps);
    return selected;
}

std::string modify_segment_uri(const std::string& uri, int new_bitrate) {
    // Find the pattern: /video/vid-XXX-seg-YYY.m4s
    size_t video_pos = uri.find("/video/");
    if (video_pos == std::string::npos) {
        return uri;
    }
    
    size_t vid_pos = uri.find("vid-", video_pos);
    if (vid_pos == std::string::npos) {
        return uri;
    }
    
    size_t seg_pos = uri.find("-seg", vid_pos);
    if (seg_pos == std::string::npos) {
        return uri;
    }
    
    // Extract the bitrate part (between "vid-" and "-seg")
    size_t bitrate_start = vid_pos + 4; // after "vid-"
    size_t bitrate_end = seg_pos;
    
    std::string current_bitrate_str = uri.substr(bitrate_start, bitrate_end - bitrate_start);
    
    // Replace the bitrate part
    std::string result = uri.substr(0, bitrate_start);
    result += std::to_string(new_bitrate);
    result += uri.substr(bitrate_end);
    
    spdlog::debug("Modified URI: {} -> {} (bitrate {} -> {})", 
                 uri, result, current_bitrate_str, new_bitrate);
    
    return result;
}

bool query_load_balancer(const std::string& client_ip, std::string& server_ip, int& server_port) {
    int lb_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (lb_fd < 0) {
        spdlog::error("Failed to create socket for load balancer");
        return false;
    }
    
    // Set timeout for connect and receive
    struct timeval timeout;
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    setsockopt(lb_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(lb_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    
    struct sockaddr_in lb_addr;
    if (make_client_sockaddr(&lb_addr, lb_or_server_ip.c_str(), lb_or_server_port) < 0) {
        close(lb_fd);
        return false;
    }
    
    if (connect(lb_fd, (struct sockaddr*)&lb_addr, sizeof(lb_addr)) < 0) {
        spdlog::error("Failed to connect to load balancer {}:{}", lb_or_server_ip, lb_or_server_port);
        close(lb_fd);
        return false;
    }
    
    LoadBalancerRequest request;
    if (inet_pton(AF_INET, client_ip.c_str(), &request.client_addr) != 1) {
        spdlog::error("Invalid client IP: {}", client_ip);
        close(lb_fd);
        return false;
    }
    
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint16_t> dist(0, 65535);
    request.request_id = htons(dist(gen));
    
    if (send(lb_fd, &request, sizeof(request), 0) < 0) {
        spdlog::error("Failed to send request to load balancer");
        close(lb_fd);
        return false;
    }
    
    LoadBalancerResponse response;
    ssize_t bytes_received = recv(lb_fd, &response, sizeof(response), 0);
    
    if (bytes_received != sizeof(response)) {
        spdlog::error("Failed to receive response from load balancer (received {}/{} bytes)", 
                     bytes_received, sizeof(response));
        close(lb_fd);
        return false;
    }
    
    close(lb_fd);
    
    // Convert response to host byte order
    response.request_id = ntohs(response.request_id);
    response.videoserver_port = ntohs(response.videoserver_port);
    
    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &response.videoserver_addr, ip_str, INET_ADDRSTRLEN);
    server_ip = ip_str;
    server_port = response.videoserver_port;
    
    spdlog::debug("Load balancer returned server {}:{}", server_ip, server_port);
    return true;
}

std::string create_200_response() {
    return "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
}

// ============================================================================
// HTTP Forwarding Functions
// ============================================================================

void forward_request_to_server(ClientConnection& conn, const std::string& request_str) {
    send(conn.server_fd, request_str.c_str(), request_str.size(), 0);
}

void forward_response_to_client(ClientConnection& conn) {
    // Read headers from server
    std::string response_header = read_http_headers(conn.server_fd, conn.send_buffer);
    if (response_header.empty()) {
        spdlog::error("Failed to read response headers from server");
        return;
    }
    
    // Forward headers to client
    send(conn.client_fd, response_header.c_str(), response_header.size(), 0);
    
    // Parse response to get content length
    HTTPMessage response;
    if (!parse_http_message(response_header, response)) {
        spdlog::error("Failed to parse server response");
        return;
    }
    
    int content_length = response.get_content_length();
    if (content_length > 0) {
        // Forward body
        char buffer[8192];
        int remaining = content_length;
        while (remaining > 0) {
            int to_read = std::min(remaining, (int)sizeof(buffer));
            int n = recv(conn.server_fd, buffer, to_read, 0);
            if (n <= 0) {
                spdlog::error("Failed to read response body from server");
                break;
            }
            send(conn.client_fd, buffer, n, 0);
            remaining -= n;
        }
    }
}

// ============================================================================
// Main Proxy Logic
// ============================================================================

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
    int request_content_length = request.get_content_length();
    if (request_content_length > 0) {
        request.body.resize(request_content_length);
        if (!read_exact(conn.client_fd, &request.body[0], request_content_length)) {
            spdlog::error("Failed to read request body");
            return;
        }
    }
    
    std::string client_uuid = request.get_header("x-489-uuid");
    if (client_uuid.empty()) {
        client_uuid = "unknown";
    }
    
    // Handle POST /on-fragment-received
    if (request.method == "POST" && request.uri == "/on-fragment-received") {
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
                    double segment_throughput_kbps = (segment_size * 8.0) / duration_ms;
                    
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
            } catch (const std::exception& e) {
                spdlog::error("Failed to parse throughput info: {}", e.what());
            }
        }
        
        std::string response = create_200_response();
        send(conn.client_fd, response.c_str(), response.size(), 0);
        return;
    }
    
    // Handle manifest requests
    if (is_manifest_request(request.uri)) {
        std::string video_key = extract_video_key(request.uri);
        
        // Check if we need to fetch the full manifest
        if (video_cache.find(video_key) == video_cache.end()) {
            spdlog::info("First time seeing video: {}, fetching full manifest", video_key);
            
            // Request the regular manifest file to parse it
            std::string original_request = request.to_string();
            forward_request_to_server(conn, original_request);
            
            // Read response
            std::string response_header = read_http_headers(conn.server_fd, conn.send_buffer);
            HTTPMessage manifest_response;
            if (!parse_http_message(response_header, manifest_response)) {
                spdlog::error("Failed to parse manifest response");
                return;
            }
            
            int manifest_content_length = manifest_response.get_content_length();
            if (manifest_content_length > 0) {
                manifest_response.body.resize(manifest_content_length);
                if (!read_exact(conn.server_fd, &manifest_response.body[0], manifest_content_length)) {
                    spdlog::error("Failed to read manifest body");
                    return;
                }
                
                // Parse and cache bitrates
                VideoInfo info;
                info.video_path = video_key;
                info.bitrates = parse_manifest_bitrates(manifest_response.body);
                video_cache[video_key] = info;
                
                spdlog::info("Cached {} bitrates for {}", info.bitrates.size(), video_key);
            } else {
                spdlog::error("Manifest response has no content");
            }
        }
        
        // Now request the no-list version for the client
        std::string no_list_uri = request.uri;
        size_t mpd_pos = no_list_uri.find("vid.mpd");
        if (mpd_pos != std::string::npos) {
            no_list_uri.replace(mpd_pos, 7, "vid-no-list.mpd");
        } else {
            spdlog::warn("Could not find 'vid.mpd' in URI: {}", request.uri);
        }
        
        request.uri = no_list_uri;
        std::string modified_request = request.to_string();
        forward_request_to_server(conn, modified_request);
        forward_response_to_client(conn);
        
        spdlog::info("Manifest requested by {} forwarded to {}:{} for {}",
                    client_uuid.empty() ? "unknown" : client_uuid, 
                    conn.server_ip, conn.server_port, no_list_uri);
        return;
    }
    
    // Handle video segment requests
    if (is_video_segment_request(request.uri)) {
        std::string video_key = extract_video_key(request.uri);
        
        auto it = video_cache.find(video_key);
        if (it != video_cache.end() && !it->second.bitrates.empty()) {
            double current_throughput = 0.0;
            if (client_uuid != "unknown") {
                auto tput_it = client_throughputs.find(client_uuid);
                if (tput_it != client_throughputs.end() && tput_it->second.initialized) {
                    current_throughput = tput_it->second.avg_throughput_kbps;
                }
            }
            
            int selected_bitrate = select_bitrate(it->second.bitrates, current_throughput);
            std::string original_uri = request.uri;
            std::string modified_uri = modify_segment_uri(request.uri, selected_bitrate);
            
            request.uri = modified_uri;
            std::string modified_request = request.to_string();
            forward_request_to_server(conn, modified_request);
            forward_response_to_client(conn);
            
            spdlog::info("Segment requested by {} forwarded to {}:{} as {} at bitrate {} Kbps",
                        client_uuid, conn.server_ip, conn.server_port, modified_uri, selected_bitrate);
            return;
        } else {
            spdlog::warn("No cached bitrates for video: {}, forwarding as-is", video_key);
        }
    }
    
    // Handle all other requests - forward as-is
    std::string request_text = request.to_string();
    forward_request_to_server(conn, request_text);
    forward_response_to_client(conn);
}

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
        spdlog::error("Failed to create server address");
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
    
    // Set SO_REUSEADDR
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
    
    // Main event loop
    while (true) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        
        FD_SET(listen_fd, &read_fds);
        int max_fd = listen_fd;
        
        for (const auto& pair : connections) {
            FD_SET(pair.second.client_fd, &read_fds);
            if (pair.second.client_fd > max_fd) {
                max_fd = pair.second.client_fd;
            }
            FD_SET(pair.second.server_fd, &read_fds);
            if (pair.second.server_fd > max_fd) {
                max_fd = pair.second.server_fd;
            }
        }
        
        int activity = select(max_fd + 1, &read_fds, nullptr, nullptr, nullptr);
        if (activity < 0) {
            spdlog::error("select() failed");
            break;
        }
        
        if (FD_ISSET(listen_fd, &read_fds)) {
            accept_new_client(listen_fd);
        }
        
        // Process existing connections
        std::vector<int> client_fds;
        for (const auto& pair : connections) {
            client_fds.push_back(pair.first);
        }
        
        for (int client_fd : client_fds) {
            auto it = connections.find(client_fd);
            if (it == connections.end()) {
                continue;
            }
            
            if (FD_ISSET(client_fd, &read_fds)) {
                handle_client_request(it->second);
            }
            
            // Also check for server responses
            if (FD_ISSET(it->second.server_fd, &read_fds)) {
                // For simplicity, we'll handle server responses in the client request handling
                // This ensures proper ordering
            }
        }
    }
    
    close(listen_fd);
    return 0;
}