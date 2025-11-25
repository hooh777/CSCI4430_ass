# Quick Reference: miProxy Implementation

## Request Processing Logic

### The Main Loop (from T04 pattern)
```cpp
while (true) {
    FD_ZERO(&read_fds);
    FD_SET(listen_fd, &read_fds);
    
    // Add all client sockets
    for (const auto& pair : connections) {
        FD_SET(pair.second.client_fd, &read_fds);
    }
    
    select(max_fd + 1, &read_fds, NULL, NULL, NULL);
    
    // New connection?
    if (FD_ISSET(listen_fd, &read_fds)) {
        accept_new_client(listen_fd);
    }
    
    // Process ONE request per ready client
    for (each client_fd) {
        if (FD_ISSET(client_fd, &read_fds)) {
            handle_client_request(connection);
        }
    }
}
```

## The Four Request Types

### Type A: Manifest (.mpd)
```
Client -> Proxy: GET /videos/tears-of-steel/vid.mpd

If first time for this video:
    Proxy -> Server: GET /videos/tears-of-steel/vid.mpd
    Server -> Proxy: [XML content with bitrates]
    Proxy: Parse and cache bitrates

Proxy -> Server: GET /videos/tears-of-steel/vid-no-list.mpd
Server -> Proxy: [no-list manifest]
Proxy -> Client: [forward no-list manifest]

Log: "Manifest requested by {uuid} forwarded to {ip}:{port} for {no-list.mpd}"
```

### Type B: Video Segment (.m4s)
```
Client -> Proxy: GET /videos/vid/video/vid-500-seg-1.m4s
                 X-489-UUID: abc-123-def

Proxy: 
    1. Get current throughput for UUID "abc-123-def"
    2. Calculate: max_bitrate = throughput / 1.5
    3. Select highest available bitrate <= max_bitrate
    4. Modify URI: vid-500-seg-1.m4s -> vid-800-seg-1.m4s

Proxy -> Server: GET /videos/vid/video/vid-800-seg-1.m4s
Server -> Proxy: [video data]
Proxy -> Client: [forward video data unchanged]

Log: "Segment requested by {uuid} forwarded to {ip}:{port} as {modified_uri} at bitrate {800} Kbps"
```

### Type C: Throughput Update (POST)
```
Client -> Proxy: POST /on-fragment-received
                 X-489-UUID: abc-123-def
                 x-fragment-size: 344912
                 x-timestamp-start: 1738472727213
                 x-timestamp-end: 1738472728625

Proxy:
    1. duration_ms = end - start = 1412 ms
    2. T_new = (344912 * 8) / 1412 = 1954 Kbps
    3. If first segment for this UUID:
         T_cur = T_new
       Else:
         T_cur = α * T_new + (1 - α) * T_cur
    4. Store T_cur for this UUID

Proxy -> Client: HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n
(DON'T forward to server!)

Log: "Client {uuid} finished receiving a segment of size {344912} bytes in {1412} ms. 
      Throughput: {1954} Kbps. Avg Throughput: {1954} Kbps"
```

### Type D: Everything Else
```
Client -> Proxy: GET /index.html (or CSS, JS, audio, etc.)
Proxy -> Server: [forward unchanged]
Server -> Proxy: [response]
Proxy -> Client: [forward unchanged]

No special logging required (info level)
```

## Data Flow Diagram

```
Browser Tab (Client)
    |
    | Multiple TCP connections (client sockets)
    |
    v
+------------------+
|    miProxy       |
|                  |
| - select() loop  |
| - HTTP parser    |
| - Bitrate logic  |
| - EWMA tracking  |
+------------------+
    |
    | One TCP connection per client socket
    |
    v
Video Server(s)
```

## State Tracking

### Per Client Socket (TCP Connection)
```cpp
struct ClientConnection {
    int client_fd;      // Browser socket
    int server_fd;      // Video server socket
    string server_ip;   // Which server
    int server_port;
    // Plus buffers for partial data
};
```

### Per Client (Browser Tab) - UUID Based
```cpp
struct ClientThroughput {
    double avg_throughput_kbps;  // EWMA estimate
    bool initialized;            // First segment?
};

// Key is X-489-UUID header value
map<string, ClientThroughput> client_throughputs;
```

### Per Video
```cpp
struct VideoInfo {
    vector<int> bitrates;  // [300, 500, 800, 1200, 2000] Kbps
    string video_path;     // "/videos/tears-of-steel"
};

// Key is video path (directory of .mpd)
map<string, VideoInfo> video_cache;
```

## HTTP Parsing Steps

### 1. Read Headers
```cpp
string headers;
char byte;
while (true) {
    recv(sockfd, &byte, 1, 0);
    headers += byte;
    
    // Check for \r\n\r\n
    if (headers.ends_with("\r\n\r\n")) {
        break;
    }
}
```

### 2. Parse Start Line
```
Request:  GET /path HTTP/1.1
Response: HTTP/1.1 200 OK
```

### 3. Parse Headers (Case-Insensitive!)
```
Content-Length: 1234
content-length: 1234  <- Same header!
X-489-UUID: abc-123
x-489-uuid: abc-123   <- Same header!
```

### 4. Read Body (If Content-Length > 0)
```cpp
int length = get_content_length();
if (length > 0) {
    body.resize(length);
    read_exact(sockfd, &body[0], length);
}
```

## Bitrate Selection Algorithm

### Input
- Available bitrates: `[300, 500, 800, 1200, 2000]` Kbps
- Current throughput: `1300` Kbps

### Calculation
```
Threshold = 1300 / 1.5 = 866.67 Kbps

Check each bitrate:
    300 < 866.67? Yes -> Select 300
    500 < 866.67? Yes -> Select 500  
    800 < 866.67? Yes -> Select 800  <- Highest that fits
    1200 < 866.67? No -> Stop

Selected: 800 Kbps
```

### Special Cases
```cpp
// First segment (no throughput yet)
if (throughput == 0.0) {
    return bitrates[0];  // Lowest
}

// No bitrate satisfies threshold
if (all_too_high) {
    return bitrates[0];  // Lowest
}
```

## EWMA Calculation

### Formula
```
T_cur = α * T_new + (1 - α) * T_cur
```

### Example with α = 0.5
```
Segment 1: T_new = 2000 Kbps
    T_cur = 2000 (first segment)

Segment 2: T_new = 1500 Kbps
    T_cur = 0.5 * 1500 + 0.5 * 2000 = 1750 Kbps

Segment 3: T_new = 1800 Kbps
    T_cur = 0.5 * 1800 + 0.5 * 1750 = 1775 Kbps
```

### Alpha Values
```
α = 0.2  -> Smooth (slow to adapt)
α = 0.5  -> Balanced
α = 0.8  -> Responsive (quick to adapt)
```

## Load Balancer Protocol

### Request (from miProxy to loadBalancer)
```cpp
struct LoadBalancerRequest {
    in_addr_t client_addr;  // Client IP (network order)
    uint16_t request_id;    // Random ID (network order)
};
```

### Response (from loadBalancer to miProxy)
```cpp
struct LoadBalancerResponse {
    in_addr_t videoserver_addr;  // Server IP (network order)
    uint16_t videoserver_port;   // Server port (network order)
    uint16_t request_id;         // Echo of request_id
};
```

### Byte Order Conversions
```cpp
// Sending
request.request_id = htons(random_id);

// Receiving
int port = ntohs(response.videoserver_port);
```

## Command Line Arguments

### Without Load Balancer
```bash
./miProxy -l 9000 -h 127.0.0.1 -p 8000 -a 0.5

-l 9000         # Proxy listens on port 9000
-h 127.0.0.1    # Video server IP
-p 8000         # Video server port
-a 0.5          # EWMA alpha = 0.5
```

### With Load Balancer
```bash
./miProxy -b -l 9000 -h 127.0.0.1 -p 8000 -a 0.5

-b              # Enable load balancing
-l 9000         # Proxy listens on port 9000
-h 127.0.0.1    # Load balancer IP (not server!)
-p 8000         # Load balancer port (not server!)
-a 0.5          # EWMA alpha = 0.5
```

## Testing Commands

### Start Video Server
```bash
cd videoserver
uv run launch_videoservers.py -p 8000
```

### Start Proxy (No LB)
```bash
./build/bin/miProxy -l 9000 -h 127.0.0.1 -p 8000 -a 0.5
```

### Test with Browser
```
Open: http://127.0.0.1:9000/index.html
Click: Tears of Steel or CUHK video
Watch: Logs in terminal
```

### Test with curl
```bash
# Get manifest
curl -v http://127.0.0.1:9000/videos/tears-of-steel/vid.mpd

# Get segment
curl -v http://127.0.0.1:9000/videos/tears-of-steel/video/vid-500-seg-1.m4s -o /dev/null
```

## Expected Log Output

```
[info] miProxy started
[info] New client socket connected with 127.0.0.1:54321 on sockfd 4
[info] Manifest requested by 880e8e87-d503-43a1-ba6e-1fc06da4aaa2 forwarded to 127.0.0.1:8000 for /videos/tears-of-steel/vid-no-list.mpd
[info] Segment requested by 880e8e87-d503-43a1-ba6e-1fc06da4aaa2 forwarded to 127.0.0.1:8000 as /videos/tears-of-steel/video/vid-500-seg-1.m4s at bitrate 500 Kbps
[info] Client 880e8e87-d503-43a1-ba6e-1fc06da4aaa2 finished receiving a segment of size 344912 bytes in 1412 ms. Throughput: 1954 Kbps. Avg Throughput: 1954 Kbps
[info] Segment requested by 880e8e87-d503-43a1-ba6e-1fc06da4aaa2 forwarded to 127.0.0.1:8000 as /videos/tears-of-steel/video/vid-800-seg-2.m4s at bitrate 800 Kbps
...
[info] Client socket sockfd 4 disconnected
```

## Quick Debug Checklist

- [ ] Video server running?
- [ ] Proxy compiled without errors?
- [ ] Correct port numbers?
- [ ] Browser pointed to proxy (not server)?
- [ ] Video files extracted?
- [ ] Dependencies installed?
- [ ] Check browser DevTools Network tab
- [ ] Check proxy logs for errors
- [ ] Try with curl first
- [ ] Test without throttling first

---

**Remember**: Simple, correct, well-tested beats clever! 🎯
