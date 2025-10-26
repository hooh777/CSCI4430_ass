#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <queue>
#include <algorithm>
#include <cstring>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <climits>

#include <spdlog/spdlog.h>
#include <cxxopts.hpp>

#include "../common/LoadBalancerProtocol.h"
#include "../common/network_utils.h"

class LoadBalancer {
private:
    int port_;
    bool geo_mode_;
    bool rr_mode_;
    std::string servers_file_;
    
    // For round-robin mode
    std::vector<std::pair<std::string, int>> servers_;
    size_t current_server_index_;
    
    // For geographic mode
    struct Node {
        std::string type;
        std::string ip;
    };
    std::unordered_map<int, Node> nodes_;
    std::unordered_map<int, std::vector<std::pair<int, int>>> graph_;
    std::unordered_map<std::string, int> ip_to_node_id_;

public:
    LoadBalancer(int port, bool geo_mode, bool rr_mode, const std::string& servers_file)
        : port_(port), geo_mode_(geo_mode), rr_mode_(rr_mode), servers_file_(servers_file), current_server_index_(0) {}
    
    bool parse_round_robin_file() {
        std::ifstream file(servers_file_);
        if (!file.is_open()) {
            spdlog::error("Failed to open servers file: {}", servers_file_);
            return false;
        }
        
        std::string line;
        int num_servers = 0;
        bool found_num_servers = false;
        
        while (std::getline(file, line)) {
            // Skip empty lines and comments
            if (line.empty() || line[0] == '#') {
                continue;
            }
            
            if (line.find("NUM_SERVERS:") == 0) {
                num_servers = std::stoi(line.substr(12));
                found_num_servers = true;
            } else {
                std::istringstream iss(line);
                std::string ip;
                int port;
                if (iss >> ip >> port) {
                    servers_.emplace_back(ip, port);
                }
            }
        }
        
        if (!found_num_servers) {
            spdlog::error("NUM_SERVERS not found in file");
            return false;
        }
        
        if (servers_.size() != static_cast<size_t>(num_servers)) {
            spdlog::error("Expected {} servers, but found {}", num_servers, servers_.size());
            return false;
        }
        
        spdlog::info("Loaded {} servers for round-robin mode", servers_.size());
        return true;
    }
    
    bool parse_geographic_file() {
        std::ifstream file(servers_file_);
        if (!file.is_open()) {
            spdlog::error("Failed to open servers file: {}", servers_file_);
            return false;
        }
        
        std::string line;
        int num_nodes = 0;
        int num_links = 0;
        bool found_num_nodes = false;
        bool found_num_links = false;
        
        // First pass: find NUM_NODES
        while (std::getline(file, line)) {
            if (line.empty() || line[0] == '#') {
                continue;
            }
            if (line.find("NUM_NODES:") == 0) {
                num_nodes = std::stoi(line.substr(10));
                found_num_nodes = true;
                break;
            }
        }
        
        if (!found_num_nodes) {
            spdlog::error("NUM_NODES not found in file");
            return false;
        }
        
        // Reset file and parse nodes
        file.clear();
        file.seekg(0, std::ios::beg);
        
        // Skip to NUM_NODES line
        while (std::getline(file, line)) {
            if (line.find("NUM_NODES:") == 0) {
                break;
            }
        }
        
        // Parse nodes
        for (int i = 0; i < num_nodes; ++i) {
            if (!std::getline(file, line)) {
                spdlog::error("Unexpected end of file while reading nodes");
                return false;
            }
            
            if (line.empty() || line[0] == '#') {
                i--; // Skip this line and try again
                continue;
            }
            
            std::istringstream iss(line);
            int node_id;
            std::string type, ip;
            if (iss >> node_id >> type >> ip) {
                nodes_[node_id] = {type, ip};
                if (ip != "NO_IP") {
                    ip_to_node_id_[ip] = node_id;
                    spdlog::debug("Mapped IP {} to node ID {} (type: {})", ip, node_id, type);
                }
            } else {
                spdlog::error("Failed to parse node line: '{}'", line);
                return false;
            }
        }
        
        // Find NUM_LINKS
        while (std::getline(file, line)) {
            if (line.empty() || line[0] == '#') {
                continue;
            }
            if (line.find("NUM_LINKS:") == 0) {
                num_links = std::stoi(line.substr(10));
                found_num_links = true;
                break;
            }
        }
        
        if (!found_num_links) {
            spdlog::error("NUM_LINKS not found in file");
            return false;
        }
        
        // Parse links
        for (int i = 0; i < num_links; ++i) {
            if (!std::getline(file, line)) {
                spdlog::error("Unexpected end of file while reading links");
                return false;
            }
            
            if (line.empty() || line[0] == '#') {
                i--; // Skip this line and try again
                continue;
            }
            
            std::istringstream iss(line);
            int from, to, cost;
            if (iss >> from >> to >> cost) {
                graph_[from].emplace_back(to, cost);
                graph_[to].emplace_back(from, cost);
                spdlog::debug("Added link: {} -> {} (cost: {})", from, to, cost);
            } else {
                spdlog::error("Failed to parse link line: '{}'", line);
                return false;
            }
        }
        
        spdlog::info("Loaded geographic network with {} nodes and {} links", num_nodes, num_links);
        
        // Debug: print all mapped IPs
        for (const auto& [ip, node_id] : ip_to_node_id_) {
            spdlog::debug("IP mapping: {} -> node {}", ip, node_id);
        }
        
        return true;
    }
    
    std::vector<int> dijkstra(int start) {
        std::vector<int> dist(nodes_.size() + 1, INT_MAX);
        std::vector<bool> visited(nodes_.size() + 1, false);
        dist[start] = 0;
        
        for (size_t i = 1; i <= nodes_.size(); ++i) {
            int u = -1;
            for (size_t j = 1; j <= nodes_.size(); ++j) {
                if (!visited[j] && (u == -1 || dist[j] < dist[u])) {
                    u = j;
                }
            }
            
            if (dist[u] == INT_MAX) {
                break;
            }
            
            visited[u] = true;
            
            if (graph_.find(u) != graph_.end()) {
                for (const auto& edge : graph_[u]) {
                    int v = edge.first;
                    int weight = edge.second;
                    if (dist[u] + weight < dist[v]) {
                        dist[v] = dist[u] + weight;
                    }
                }
            }
        }
        
        return dist;
    }
    
    std::pair<std::string, int> find_closest_server(const std::string& client_ip) {
        spdlog::debug("Finding closest server for client IP: {}", client_ip);
        
        // Find the node ID for the client IP
        auto client_it = ip_to_node_id_.find(client_ip);
        if (client_it == ip_to_node_id_.end()) {
            spdlog::error("Client IP {} not found in network", client_ip);
            return {"", 0};
        }
        
        int client_node = client_it->second;
        spdlog::debug("Client {} is at node {}", client_ip, client_node);
        
        // Run Dijkstra from client node
        auto distances = dijkstra(client_node);
        
        // Find the closest video server
        int min_distance = INT_MAX;
        int best_server_node = -1;
        
        for (int node_id = 1; node_id <= static_cast<int>(nodes_.size()); ++node_id) {
            if (nodes_.find(node_id) != nodes_.end() && 
                nodes_[node_id].type == "SERVER" && 
                distances[node_id] < min_distance) {
                min_distance = distances[node_id];
                best_server_node = node_id;
            }
        }
        
        if (best_server_node == -1) {
            spdlog::error("No server found in network");
            return {"", 0};
        }
        
        spdlog::debug("Closest server is node {} with distance {}", best_server_node, min_distance);
        
        // Find the IP and port for the server node
        // In geographic mode, we need to find a server that connects to this node
        for (const auto& [node_id, node] : nodes_) {
            if (node.type == "SERVER") {
                // For simplicity, assume server nodes have IPs like 10.0.0.X:8000
                // We'll use the node ID to construct the server address
                std::string server_ip = "10.0.0." + std::to_string(node_id);
                int server_port = 8000;
                
                // Check if this is the closest server
                if (node_id == best_server_node) {
                    spdlog::info("Client {} assigned to server {}:{} (distance: {})", 
                                client_ip, server_ip, server_port, min_distance);
                    return {server_ip, server_port};
                }
            }
        }
        
        spdlog::error("Failed to find server details for node {}", best_server_node);
        return {"", 0};
    }
    
    std::pair<std::string, int> get_next_server_round_robin() {
        if (servers_.empty()) {
            return {"", 0};
        }
        
        auto server = servers_[current_server_index_];
        current_server_index_ = (current_server_index_ + 1) % servers_.size();
        
        return server;
    }
    
    void handle_request(int client_fd, const LoadBalancerRequest& request) {
        // Convert client IP to string
        char client_ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &request.client_addr, client_ip_str, INET_ADDRSTRLEN);
        
        spdlog::info("Received request for client {} with request ID {}", 
                    client_ip_str, ntohs(request.request_id));
        
        LoadBalancerResponse response;
        response.request_id = request.request_id;
        
        std::pair<std::string, int> server_info;
        
        if (geo_mode_) {
            server_info = find_closest_server(client_ip_str);
        } else {
            server_info = get_next_server_round_robin();
        }
        
        if (server_info.first.empty()) {
            spdlog::error("Failed to find server for client {}", client_ip_str);
            close(client_fd);
            return;
        }
        
        // Convert server IP to binary form
        if (inet_pton(AF_INET, server_info.first.c_str(), &response.videoserver_addr) != 1) {
            spdlog::error("Invalid server IP: {}", server_info.first);
            close(client_fd);
            return;
        }
        
        response.videoserver_port = htons(server_info.second);
        
        // Send response
        if (send(client_fd, &response, sizeof(response), 0) < 0) {
            spdlog::error("Failed to send response to client");
        } else {
            spdlog::info("Responded to request ID {} with server {}:{}", 
                        ntohs(request.request_id), server_info.first, server_info.second);
        }
        
        close(client_fd);
    }
    
    void run() {
        // Parse configuration file
        bool parse_success;
        if (geo_mode_) {
            parse_success = parse_geographic_file();
        } else {
            parse_success = parse_round_robin_file();
        }
        
        if (!parse_success) {
            spdlog::error("Failed to parse configuration file");
            return;
        }
        
        // Create listening socket
        int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd < 0) {
            spdlog::error("Failed to create listening socket");
            return;
        }
        
        // Set SO_REUSEADDR
        int opt = 1;
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        
        // Bind
        struct sockaddr_in server_addr;
        make_server_sockaddr(&server_addr, port_);
        if (bind(listen_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
            spdlog::error("Failed to bind to port {}", port_);
            close(listen_fd);
            return;
        }
        
        // Listen
        if (listen(listen_fd, 50) < 0) {
            spdlog::error("Failed to listen");
            close(listen_fd);
            return;
        }
        
        spdlog::info("Load balancer started on port {}", port_);
        
        // Main event loop
        while (true) {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            
            int client_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &client_len);
            if (client_fd < 0) {
                spdlog::error("Failed to accept client connection");
                continue;
            }
            
            // Receive request
            LoadBalancerRequest request;
            ssize_t bytes_received = recv(client_fd, &request, sizeof(request), 0);
            
            if (bytes_received != sizeof(request)) {
                spdlog::error("Failed to receive complete request (received {}/{} bytes)", 
                             bytes_received, sizeof(request));
                close(client_fd);
                continue;
            }
            
            handle_request(client_fd, request);
        }
        
        close(listen_fd);
    }
};

int main(int argc, char* argv[]) {
    cxxopts::Options options("loadBalancer", "Load balancer for video servers");
    options.add_options()
        ("p,port", "Port to listen on", cxxopts::value<int>())
        ("g,geo", "Use geographic mode", cxxopts::value<bool>()->default_value("false"))
        ("r,round-robin", "Use round-robin mode", cxxopts::value<bool>()->default_value("false"))
        ("s,servers", "Servers configuration file", cxxopts::value<std::string>())
        ("help", "Print help");
    
    auto result = options.parse(argc, argv);
    
    if (result.count("help")) {
        std::cout << options.help() << std::endl;
        return 0;
    }
    
    // Validate arguments
    if (!result.count("port") || !result.count("servers")) {
        spdlog::error("Missing required arguments");
        std::cout << options.help() << std::endl;
        return 1;
    }
    
    int port = result["port"].as<int>();
    bool geo_mode = result["geo"].as<bool>();
    bool rr_mode = result["round-robin"].as<bool>();
    std::string servers_file = result["servers"].as<std::string>();
    
    if (port < 1024 || port > 65535) {
        spdlog::error("Port must be in range [1024, 65535]");
        return 1;
    }
    
    if (!geo_mode && !rr_mode) {
        spdlog::error("Must specify either --geo or --round-robin");
        return 1;
    }
    
    if (geo_mode && rr_mode) {
        spdlog::error("Cannot specify both --geo and --round-robin");
        return 1;
    }
    
    LoadBalancer lb(port, geo_mode, rr_mode, servers_file);
    lb.run();
    
    return 0;
}