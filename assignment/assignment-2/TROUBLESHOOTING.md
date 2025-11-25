# Common Issues and Fixes

## Issue 1: Manifest Parsing Fails

### Symptom
```
[error] Failed to parse manifest XML
```

### Cause
The XML structure might be different than expected.

### Fix
Check the actual manifest structure:
```bash
curl http://127.0.0.1:8000/videos/tears-of-steel/vid.mpd
```

Adjust the XPath in `parse_manifest_bitrates()` function if needed.

### Quick Debug
Add before parsing:
```cpp
spdlog::debug("Manifest content: {}", manifest_content.substr(0, 500));
```

---

## Issue 2: Bitrate Never Changes

### Symptom
Always selects same bitrate regardless of throttling.

### Possible Causes
1. Throughput not being updated
2. UUID not being captured
3. Bitrates not parsed correctly

### Debug Steps
1. Check if throughput logs appear:
   ```
   [info] Client X finished receiving a segment...
   ```

2. Add debug log in select_bitrate():
   ```cpp
   spdlog::debug("Selecting bitrate: throughput={}, bitrates available={}",
                 throughput_kbps, bitrates.size());
   ```

3. Verify UUID in request:
   ```cpp
   spdlog::debug("Request UUID: {}", client_uuid);
   ```

---

## Issue 3: Connection Refused to Video Server

### Symptom
```
[error] Failed to connect to video server 127.0.0.1:8000
```

### Solutions
1. Verify video server is running:
   ```bash
   curl http://127.0.0.1:8000/
   ```

2. Check port conflicts:
   ```bash
   lsof -i :8000
   ```

3. Try different port or restart server

---

## Issue 4: Multiple Clients Interfere with Each Other

### Symptom
One client stops working when another connects.

### Likely Cause
Socket FDs getting mixed up in select() loop.

### Fix
Make sure to iterate over a copy of connections:
```cpp
std::vector<int> client_fds;
for (const auto& pair : connections) {
    client_fds.push_back(pair.first);
}

for (int fd : client_fds) {
    if (connections.find(fd) != connections.end()) {
        // Process...
    }
}
```

Already implemented in the code ✅

---

## Issue 5: Video Stutters or Buffers

### Possible Causes
1. Bitrate selection too aggressive
2. Throughput calculation inaccurate
3. Network throttling too severe

### Solutions
1. **Lower alpha** (more smoothing):
   ```bash
   ./miProxy ... -a 0.2
   ```

2. **Adjust bitrate threshold** (conservative):
   Change `1.5` to `2.0` in select_bitrate():
   ```cpp
   if (throughput_kbps >= 2.0 * bitrate) {
   ```

3. **Check actual throughput**:
   Look at logs to see if measured throughput matches expected.

---

## Issue 6: Load Balancer Returns Wrong Server

### Symptom
Gets server IP but can't connect, or wrong response format.

### Debug
1. Test load balancer with queryLoadBalancer:
   ```bash
   ./util/queryLoadBalancer -i 10.0.0.1 -h 127.0.0.1 -p 9000
   ```

2. Add debug in query_load_balancer():
   ```cpp
   spdlog::debug("LB returned: {}:{}", server_ip, server_port);
   ```

3. Verify byte order conversions:
   - request_id should use `htons()` before sending
   - videoserver_port should use `ntohs()` after receiving

---

## Issue 7: Headers Not Found (Case Sensitivity)

### Symptom
```
Content-Length: 0 (but there is content)
```

### Cause
Header key not lowercase.

### Already Fixed ✅
HTTPMessage.h uses `to_lower()` for all header keys.

But double-check your headers:
```cpp
std::string uuid = request.get_header("X-489-UUID");  // Works
std::string uuid = request.get_header("x-489-uuid");  // Also works
```

---

## Issue 8: Body Not Fully Transferred

### Symptom
Video plays for a bit then stops, or gets corrupted.

### Cause
Not reading/writing full Content-Length.

### Fix
Already implemented with chunked reading:
```cpp
while (remaining > 0) {
    int to_read = std::min(remaining, (int)sizeof(buffer));
    int n = recv(server_fd, buffer, to_read, 0);
    if (n <= 0) break;
    send(client_fd, buffer, n, 0);
    remaining -= n;
}
```

But verify by adding:
```cpp
spdlog::debug("Transferred {} bytes", content_length);
```

---

## Issue 9: Memory Leak or Growing Memory Usage

### Symptom
Memory usage increases over time.

### Likely Causes
1. Not closing sockets properly
2. Buffers growing unbounded
3. Video cache not cleaned

### Solutions
1. **Verify cleanup on disconnect**:
   ```cpp
   close(conn.client_fd);
   close(conn.server_fd);
   connections.erase(conn.client_fd);
   server_to_client.erase(conn.server_fd);
   ```

2. **Clear buffers** after use (already done ✅):
   ```cpp
   leftover_buffer.clear();
   ```

3. **Monitor with valgrind** (if on Linux):
   ```bash
   valgrind --leak-check=full ./bin/miProxy ...
   ```

---

## Issue 10: Compilation Errors

### Missing spdlog
```
fatal error: spdlog/spdlog.h: No such file or directory
```

**Fix**: Run `./download_deps.sh`

### Missing Boost
```
Could not find a package configuration file provided by "Boost"
```

**Fix**:
```bash
# Mac
brew install boost

# Ubuntu/WSL
sudo apt-get install libboost-all-dev
```

### CMake too old
```
CMake 3.30 or higher is required
```

**Fix**:
```bash
# Mac
brew upgrade cmake

# Ubuntu
# Download from cmake.org
```

---

## Performance Optimizations (Bonus)

### 1. Non-blocking Sockets
```cpp
// Make socket non-blocking
int flags = fcntl(sockfd, F_GETFL, 0);
fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
```

### 2. Increase File Descriptor Limit
```bash
ulimit -n 4096
```

Or in code:
```cpp
#include <sys/resource.h>

struct rlimit limit;
limit.rlim_cur = 4096;
limit.rlim_max = 4096;
setrlimit(RLIMIT_NOFILE, &limit);
```

### 3. Larger Listen Backlog
Already set to 50:
```cpp
listen(listen_fd, 50);
```

Could increase to 128 or more.

---

## Testing Tips

### 1. Use curl for Quick Tests
```bash
# Test manifest
curl -v http://127.0.0.1:9000/videos/tears-of-steel/vid.mpd

# Test segment
curl -v http://127.0.0.1:9000/videos/tears-of-steel/video/vid-500-seg-1.m4s -o /dev/null
```

### 2. Monitor Connections
```bash
# See all connections to proxy
netstat -an | grep 9000

# See proxy connections to video server
netstat -an | grep 8000
```

### 3. Stress Test
```bash
# Open many connections
for i in {1..20}; do
    curl http://127.0.0.1:9000/index.html &
done
```

### 4. Log Everything
```bash
# Redirect logs to file
./bin/miProxy ... 2>&1 | tee miproxy.log
```

---

## Quick Fixes Reference

| Problem | Quick Fix |
|---------|-----------|
| Port in use | `lsof -ti:9000 \| xargs kill -9` |
| Can't find headers | Check `#include` paths |
| Segfault | Add null checks, use debugger |
| Wrong bitrate | Adjust threshold or alpha |
| Video won't play | Check browser console |
| Logs missing | Use `spdlog::info()` not `cout` |
| Build fails | Clean: `rm -rf build && mkdir build` |

---

## When to Ask for Help

1. ✅ You've read the error message carefully
2. ✅ You've checked the logs
3. ✅ You've tested with curl
4. ✅ You've checked browser DevTools
5. ✅ You've added debug logs
6. ✅ You've googled the error
7. ✅ You've checked this document

Then ask your teammate or TA! 😊

---

Good luck with Part 1! The code I've provided should work out of the box with minimal fixes needed. 🚀
