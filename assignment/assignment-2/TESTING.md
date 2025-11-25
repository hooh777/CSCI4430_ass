# miProxy - Testing Guide

## Quick Build & Test

### 1. Download Dependencies
```bash
cd assignment/assignment-2
./download_deps.sh
```

### 2. Download Video Files
Download from Google Drive links in main README:
- [Tears of Steel](https://drive.google.com/file/d/1odL-aQF9k7aZxiEtwyWktgScUZ0IO_mo/view)
- [Soar with CUHK](https://drive.google.com/file/d/1EIRRG91G2nEjJfKL4A5hU5gBMO2monyu/view)

Place in `videoserver/static/videos/` and extract:
```bash
cd videoserver/static/videos
tar -xvzf cuhk.tar.gz
tar -xvzf tears-of-steel.tar.gz
```

### 3. Build miProxy
```bash
mkdir build
cd build
cmake ../cpp
make
```

This creates `build/bin/miProxy`

### 4. Start Video Server
```bash
cd videoserver
uv sync
uv run launch_videoservers.py -n 1 -p 8000
```

### 5. Start miProxy
```bash
# From build directory
./bin/miProxy -l 9000 -h 127.0.0.1 -p 8000 -a 0.5
```

### 6. Test in Browser
Open browser and navigate to:
```
http://127.0.0.1:9000/index.html
```

Click on a video to play!

## Testing Strategy

### Phase 1: Basic Forwarding
- Start with single client
- Verify requests/responses are forwarded
- Check logs for connection messages

### Phase 2: Manifest Handling
- Watch browser request `.mpd` file
- Check logs show manifest request
- Verify bitrates are parsed (check debug logs)

### Phase 3: Bitrate Adaptation
1. **No Throttling**: Should select high bitrate
2. **Moderate Throttle** (e.g., 1.5 Mbps): Should select medium bitrate
3. **Heavy Throttle** (e.g., 500 Kbps): Should select low bitrate

### Phase 4: Multiple Clients
- Open multiple browser tabs
- Each should have independent throughput tracking
- Check UUIDs in logs

## Browser Throttling

### Chrome/Edge:
1. F12 (DevTools)
2. Network tab
3. Throttling dropdown
4. Add custom profiles with 0ms latency

### Firefox:
1. F12 (DevTools)
2. Network tab
3. Throttling dropdown

## Debugging Tips

```bash
# Verbose logging
export SPDLOG_LEVEL=debug
./bin/miProxy -l 9000 -h 127.0.0.1 -p 8000 -a 0.5
```

## Expected Logs

```
[info] miProxy started
[info] New client socket connected with 127.0.0.1:54321 on sockfd 4
[info] Manifest requested by <uuid> forwarded to 127.0.0.1:8000 for /videos/tears-of-steel/vid-no-list.mpd
[info] Segment requested by <uuid> forwarded to 127.0.0.1:8000 as /videos/tears-of-steel/video/vid-500-seg-1.m4s at bitrate 500 Kbps
[info] Client <uuid> finished receiving a segment of size 344912 bytes in 1412 ms. Throughput: 1954 Kbps. Avg Throughput: 1954 Kbps
```

## Common Issues

### "Address already in use"
```bash
# Kill process using port 9000
lsof -ti:9000 | xargs kill -9
```

### "Connection refused"
- Make sure video server is running
- Check IP/port are correct

### Video won't play
- Check browser console for errors
- Check Network tab in DevTools
- Verify manifest file is being served

## Testing Checklist

- [ ] Proxy starts and listens
- [ ] Browser can connect
- [ ] Manifest file requested and parsed
- [ ] Video segments requested
- [ ] Bitrate adapts to throttling
- [ ] Multiple clients work simultaneously
- [ ] Throughput calculated correctly
- [ ] Logs match expected format
