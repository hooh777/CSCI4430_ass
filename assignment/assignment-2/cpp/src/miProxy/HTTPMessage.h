#pragma once

#include <map>
#include <string>
#include <sstream>
#include <algorithm>
#include <cctype>

/**
 * Simple HTTP Message Parser
 * Handles both HTTP requests and responses
 */
class HTTPMessage {
public:
    std::string method;           // GET, POST, etc.
    std::string uri;              // Request URI (e.g., /videos/vid.mpd)
    std::string version;          // HTTP/1.1
    std::string status_code;      // For responses: 200, 404, etc.
    std::string status_text;      // For responses: OK, Not Found, etc.
    
    std::map<std::string, std::string> headers;  // HTTP headers (case-insensitive keys)
    std::string body;             // Message body
    
    bool is_request;              // true for request, false for response
    
    HTTPMessage() : is_request(true) {}
    
    /**
     * Parse the first line of HTTP message
     * Request: GET /path HTTP/1.1
     * Response: HTTP/1.1 200 OK
     */
    bool parse_start_line(const std::string& line) {
        std::istringstream iss(line);
        std::string first, second, third;
        iss >> first >> second >> third;
        
        if (first.find("HTTP/") == 0) {
            // This is a response
            is_request = false;
            version = first;
            status_code = second;
            // Rest of line is status text
            std::getline(iss, status_text);
            // Trim leading space
            if (!status_text.empty() && status_text[0] == ' ') {
                status_text = status_text.substr(1);
            }
        } else {
            // This is a request
            is_request = true;
            method = first;
            uri = second;
            version = third;
        }
        return true;
    }
    
    /**
     * Add a header (key is stored in lowercase for case-insensitive lookup)
     */
    void add_header(const std::string& key, const std::string& value) {
        std::string lower_key = to_lower(key);
        headers[lower_key] = value;
    }
    
    /**
     * Get header value (case-insensitive)
     */
    std::string get_header(const std::string& key) const {
        std::string lower_key = to_lower(key);
        auto it = headers.find(lower_key);
        if (it != headers.end()) {
            return it->second;
        }
        return "";
    }
    
    /**
     * Check if header exists (case-insensitive)
     */
    bool has_header(const std::string& key) const {
        std::string lower_key = to_lower(key);
        return headers.find(lower_key) != headers.end();
    }
    
    /**
     * Get content length from headers
     */
    int get_content_length() const {
        std::string cl = get_header("content-length");
        if (cl.empty()) {
            return 0;
        }
        try {
            return std::stoi(cl);
        } catch (...) {
            return 0;
        }
    }
    
    /**
     * Convert HTTP message back to string for sending
     */
    std::string to_string() const {
        std::ostringstream oss;
        
        // Start line
        if (is_request) {
            oss << method << " " << uri << " " << version << "\r\n";
        } else {
            oss << version << " " << status_code << " " << status_text << "\r\n";
        }
        
        // Headers
        for (const auto& pair : headers) {
            // Capitalize first letter of each word in header name for output
            oss << capitalize_header(pair.first) << ": " << pair.second << "\r\n";
        }
        
        // Empty line separating headers from body
        oss << "\r\n";
        
        // Body
        oss << body;
        
        return oss.str();
    }
    
private:
    /**
     * Convert string to lowercase
     */
    static std::string to_lower(const std::string& str) {
        std::string result = str;
        std::transform(result.begin(), result.end(), result.begin(),
                      [](unsigned char c) { return std::tolower(c); });
        return result;
    }
    
    /**
     * Capitalize header name (e.g., "content-length" -> "Content-Length")
     */
    static std::string capitalize_header(const std::string& str) {
        std::string result = str;
        bool capitalize_next = true;
        for (size_t i = 0; i < result.length(); i++) {
            if (capitalize_next && std::isalpha(result[i])) {
                result[i] = std::toupper(result[i]);
                capitalize_next = false;
            } else if (result[i] == '-') {
                capitalize_next = true;
            }
        }
        return result;
    }
};
