# Part 1 Implementation Checklist

## ✅ Completed

### Core Files
- [x] `HTTPMessage.h` - HTTP message parser
- [x] `miproxy.cpp` - Main proxy implementation

### Basic Functionality
- [x] Accept multiple client connections using select()
- [x] One proxy-server connection per client socket
- [x] Parse HTTP requests/responses
- [x] Forward requests and responses
- [x] Handle persistent HTTP 1.1 connections

### Manifest Handling (Type A)
- [x] Detect .mpd requests
- [x] Request regular manifest for parsing
- [x] Request no-list manifest for client
- [x] Parse XML to extract bitrates (using pugixml)
- [x] Cache bitrates per video
- [x] Log manifest requests

### Video Segment Handling (Type B)
- [x] Detect .m4s video segment requests
- [x] Get current throughput for client UUID
- [x] Select appropriate bitrate (throughput >= 1.5 * bitrate)
- [x] Modify segment URI with selected bitrate
- [x] Forward modified request to server
- [x] Log segment requests with bitrate

### Throughput Updates (Type C)
- [x] Detect POST /on-fragment-received
- [x] Extract timing headers (size, start, end)
- [x] Calculate segment throughput
- [x] Update EWMA: T_cur = α * T_new + (1 - α) * T_cur
- [x] Track per UUID (not per socket)
- [x] Respond with 200 OK (don't forward)
- [x] Log throughput info

### Other Requests (Type D)
- [x] Forward all other requests as-is
- [x] Forward responses unchanged

### Load Balancing Support
- [x] Parse -b flag
- [x] Query load balancer for new connections
- [x] Use LoadBalancerProtocol structs
- [x] Handle byte order conversions (htonl/ntohl)

### Logging
- [x] "miProxy started"
- [x] "New client socket connected..."
- [x] "Manifest requested by..."
- [x] "Segment requested by..."
- [x] "Client X finished receiving..."
- [x] "Client socket sockfd X disconnected"

### Command Line
- [x] Parse with cxxopts
- [x] -l (listen port)
- [x] -h (hostname)
- [x] -p (port)
- [x] -a (alpha)
- [x] -b (balance flag)
- [x] Validate port ranges [1024, 65535]
- [x] Validate alpha range [0, 1]

### Error Handling
- [x] Exit on invalid arguments
- [x] Handle socket errors
- [x] Clean up on disconnect
- [x] Log errors appropriately

## 📋 TODO: Testing

### Phase 1: Basic Setup
- [ ] Download dependencies (./download_deps.sh)
- [ ] Install Boost (brew install boost or apt-get)
- [ ] Download video files from Google Drive
- [ ] Extract video files
- [ ] Build project (mkdir build && cd build && cmake ../cpp && make)

### Phase 2: Single Client
- [ ] Start video server (port 8000)
- [ ] Start miProxy (port 9000, no -b flag)
- [ ] Open browser to http://127.0.0.1:9000/index.html
- [ ] Verify index page loads
- [ ] Click on video
- [ ] Check manifest log appears
- [ ] Check segment logs appear
- [ ] Check throughput logs appear
- [ ] Verify video plays smoothly

### Phase 3: Bitrate Adaptation
- [ ] Test with no throttling (should select high bitrate)
- [ ] Test with 1.5 Mbps throttle (should select medium)
- [ ] Test with 500 Kbps throttle (should select low)
- [ ] Verify bitrate changes in logs
- [ ] Verify bitrate adapts over time

### Phase 4: Multiple Clients
- [ ] Open 2-3 browser tabs
- [ ] Play different videos in each
- [ ] Verify separate UUIDs in logs
- [ ] Verify independent throughput tracking
- [ ] Check all videos play smoothly

### Phase 5: Load Balancer Integration
- [ ] Implement Part 2 (loadBalancer)
- [ ] Start multiple video servers (ports 8000-8002)
- [ ] Start load balancer with round-robin
- [ ] Start miProxy with -b flag
- [ ] Open multiple clients
- [ ] Verify different servers assigned
- [ ] Check logs show different server IPs

### Phase 6: Edge Cases
- [ ] Client disconnects mid-stream
- [ ] Rapid connect/disconnect
- [ ] Large video files
- [ ] Many simultaneous connections (10+)
- [ ] Network throttling changes mid-stream

## 🐛 Known Issues to Watch

### Potential Problems
- [ ] Manifest parsing fails on some videos
- [ ] Bitrate selection too conservative/aggressive
- [ ] EWMA not converging properly
- [ ] Multiple sockets per client not handled
- [ ] Load balancer connection fails
- [ ] Memory leaks on disconnect
- [ ] Headers not parsed case-insensitively

### Debug Steps if Issues Occur
1. Check spdlog output for errors
2. Use browser DevTools Network tab
3. Verify video server is responding
4. Test with curl/wget first
5. Add debug logs (spdlog::debug)
6. Check socket states (netstat)
7. Verify Content-Length handling

## 📊 Grading Checklist

### Functionality (main points)
- [ ] Proxy forwards requests/responses
- [ ] Handles multiple clients with select()
- [ ] Parses manifest files correctly
- [ ] Modifies segment requests appropriately
- [ ] Calculates throughput correctly
- [ ] EWMA updates properly
- [ ] Bitrate selection logic correct
- [ ] Load balancer integration works
- [ ] Logs in correct format

### Code Quality
- [ ] Compiles without warnings
- [ ] Proper error handling
- [ ] Clean code structure
- [ ] Good comments
- [ ] No memory leaks
- [ ] Handles edge cases

### Testing
- [ ] Works with provided video files
- [ ] Adapts to different network speeds
- [ ] Handles multiple simultaneous clients
- [ ] Integrates with load balancer
- [ ] Passes autograder tests

## 🚀 Submission

- [ ] Test locally thoroughly
- [ ] Run with different configurations
- [ ] Clean up debug code
- [ ] Verify all logs are correct format
- [ ] Create tarball: `bash util/submit.sh .`
- [ ] Submit to autograder
- [ ] Check autograder feedback
- [ ] Fix any issues
- [ ] Resubmit (max 10/day)

## 💡 Tips

- Use Chrome DevTools Network tab extensively
- Test with both video files (different characteristics)
- Try different alpha values (0.2, 0.5, 0.8)
- Monitor CPU/memory usage
- Test on same machine as submission
- Keep logs for debugging
- Save working version before major changes

Good luck! 🎉
