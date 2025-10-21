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
            if (line.find("NUM_SERVERS:") == 0) {
                num_servers = std::stoi(line.substr(12));
                found_num_servers = true;
            } else if (!line.empty() && line[0] != '#') {
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
        
        // Parse NUM_NODES
        while (std::getline(file, line)) {
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
        
        // Parse nodes
        for (int i = 0; i < num_nodes; ++i) {
            if (!std::getline(file, line)) {
                spdlog::error("Unexpected end of file while reading nodes");
                return false;
            }
            
            std::istringstream iss(line);
            int node_id;
            std::string type, ip;
            if (iss >> node_id >> type >> ip) {
                nodes_[node_id] = {type, ip};
                if (ip != "NO_IP") {
                    ip_to_node_id_[ip] = node_id;
                }
            }
        }
        
        // Parse NUM_LINKS
        while (std::getline(file, line)) {
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
            
            std::istringstream iss(line);
            int from, to, cost;
            if (iss >> from >> to >> cost) {
                graph_[from].emplace_back(to, cost);
                graph_[to].emplace_back(from, cost);
            }
        }
        
        spdlog::info("Loaded geographic network with {} nodes and {} links", num_nodes, num_links);
        return true;
    }
    
    std::vector<int> dijkstra(int start) {
        std::vector<int> dist(nodes_.size() + 1, INT_MAX); // +1 because node IDs might not start from 0
        std::vector<bool> visited(nodes_.size() + 1, false);
        dist[start] = 0;
        
        auto cmp = [&](const std::pair<int, int>& a, const std::pair<int, int>& b) {
            return a.second > b.second;
        };
        std::priority_queue<std::pair<int, int>, std::vector<std::pair<int, int>>, decltype(cmp)> pq(cmp);
        pq.push({start, 0});
        
        while (!pq.empty()) {
            int u = pq.top().first;
            pq.pop();
            
            if (visited[u]) continue;
            visited[u] = true;
            
            for (const auto& edge : graph_[u]) {
                int v = edge.first;
                int weight = edge.second;
                
                if (dist[u] != INT_MAX && dist[u] + weight < dist[v]) {
                    dist[v] = dist[u] + weight;
                    pq.push({v, dist[v]});
                }
            }
        }
        
        return dist;
    }
    
    std::pair<std::string, int> find_closest_server(const std::string& client_ip) {
        if (ip_to_node_id_.find(client_ip) == ip_to_node_id_.end()) {
            spdlog::warn("Client IP {} not found in network", client_ip);
            return {"", -1};
        }
        
        int client_id = ip_to_node_id_[client_ip];
        auto distances = dijkstra(client_id);
        
        int min_distance = INT_MAX;
        int best_server_id = -1;
        
        for (const auto& [node_id, node] : nodes_) {
            if (node.type == "SERVER" && distances[node_id] < min_distance) {
                min_distance = distances[node_id];
                best_server_id = node_id;
            }
        }
        
        if (best_server_id != -1 && min_distance != INT_MAX) {
            spdlog::debug("Found closest server {} at distance {}", best_server_id, min_distance);
            return {nodes_[best_server_id].ip, 8000}; // Default port for geographic mode
        }
        
        spdlog::warn("No reachable server found for client {}", client_ip);
        return {"", -1};
    }
    
    std::pair<std::string, int> get_next_round_robin_server() {
        if (servers_.empty()) {
            return {"", -1};
        }
        
        auto server = servers_[current_server_index_];
        current_server_index_ = (current_server_index_ + 1) % servers_.size();
        return server;
    }
    
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
    
    bool handle_client(int client_sock) {
        LoadBalancerRequest request;
        
        if (!read_exact(client_sock, reinterpret_cast<char*>(&request), sizeof(request))) {
            spdlog::error("Failed to read request from client");
            return false;
        }
        
        // Convert from network byte order
        request.request_id = ntohs(request.request_id);
        
        // Convert binary IP to string
        struct in_addr addr;
        addr.s_addr = request.client_addr;
        std::string client_ip = inet_ntoa(addr);
        
        spdlog::info("Received request for client {} with request ID {}", client_ip, request.request_id);
        
        std::pair<std::string, int> server_info;
        
        if (rr_mode_) {
            server_info = get_next_round_robin_server();
        } else if (geo_mode_) {
            server_info = find_closest_server(client_ip);
        }
        
        if (server_info.first.empty()) {
            spdlog::info("Failed to fulfill request ID {}", request.request_id);
            close(client_sock);
            return true;
        }
        
        LoadBalancerResponse response;
        response.request_id = htons(request.request_id);
        
        // Convert server IP string to binary
        if (inet_pton(AF_INET, server_info.first.c_str(), &response.videoserver_addr) != 1) {
            spdlog::error("Invalid server IP address: {}", server_info.first);
            return false;
        }
        
        response.videoserver_port = htons(server_info.second);
        
        if (send_data(client_sock, std::string_view(reinterpret_cast<char*>(&response), sizeof(response))) != 0) {
            spdlog::error("Failed to send response to client");
            return false;
        }
        
        spdlog::info("Responded to request ID {} with server {}:{}", 
                     request.request_id, server_info.first, server_info.second);
        return true;
    }
    
    void run() {
        if (rr_mode_ && !parse_round_robin_file()) {
            spdlog::error("Failed to parse round-robin file");
            return;
        }
        if (geo_mode_ && !parse_geographic_file()) {
            spdlog::error("Failed to parse geographic file");
            return;
        }
        
        int server_sock = socket(AF_INET, SOCK_STREAM, 0);
        if (server_sock < 0) {
            spdlog::error("Failed to create socket");
            return;
        }
        
        int opt = 1;
        if (setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
            spdlog::error("Failed to set socket options");
            close(server_sock);
            return;
        }
        
        sockaddr_in server_addr{};
        server_addr.sin_family = AF_INET;
        server_addr.sin_addr.s_addr = INADDR_ANY;
        server_addr.sin_port = htons(port_);
        
        if (bind(server_sock, (sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
            spdlog::error("Failed to bind to port {}", port_);
            close(server_sock);
            return;
        }
        
        if (listen(server_sock, 10) < 0) {
            spdlog::error("Failed to listen on socket");
            close(server_sock);
            return;
        }
        
        spdlog::info("Load balancer started on port {}", port_);
        
        while (true) {
            sockaddr_in client_addr{};
            socklen_t client_len = sizeof(client_addr);
            int client_sock = accept(server_sock, (sockaddr*)&client_addr, &client_len);
            
            if (client_sock < 0) {
                spdlog::error("Failed to accept client connection");
                continue;
            }
            
            if (!handle_client(client_sock)) {
                spdlog::error("Error handling client");
            }
            
            close(client_sock);
        }
        
        close(server_sock);
    }
};

int main(int argc, char* argv[]) {
    cxxopts::Options options("loadBalancer", "Load balancer for video CDN");
    
    options.add_options()
        ("p,port", "Port of the load balancer", cxxopts::value<int>())
        ("g,geo", "Run in geo mode", cxxopts::value<bool>()->default_value("false"))
        ("r,rr", "Run in round-robin mode", cxxopts::value<bool>()->default_value("false"))
        ("s,servers", "Path to file containing server info", cxxopts::value<std::string>())
        ("h,help", "Print usage");
    
    auto result = options.parse(argc, argv);
    
    if (result.count("help")) {
        std::cout << options.help() << std::endl;
        return 0;
    }
    
    // Error checking
    if (!result.count("port") || !result.count("servers")) {
        spdlog::error("Missing required arguments");
        return 1;
    }
    
    int port = result["port"].as<int>();
    if (port < 1024 || port > 65535) {
        spdlog::error("Port must be in range [1024, 65535]");
        return 1;
    }
    
    bool geo_mode = result["geo"].as<bool>();
    bool rr_mode = result["rr"].as<bool>();
    
    if (geo_mode == rr_mode) {
        spdlog::error("Must specify exactly one of --geo or --rr");
        return 1;
    }
    
    std::string servers_file = result["servers"].as<std::string>();
    
    LoadBalancer lb(port, geo_mode, rr_mode, servers_file);
    lb.run();
    
    return 0;
}