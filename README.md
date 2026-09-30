# camera-connector

Thin, device-local camera capture for AgentJetson.

**Responsibilities**
- Open industry camera sources: V4L2 (`/dev/video*`), RTSP, file, USB UVC
- Normalize frames (optional resize)
- Feed a low-latency FrameQueue (drop-oldest)
- **Network path:** publish `FrameEnvelope` streams over gRPC (`FrameService`)

**Does not**
- Run any ML models
- Emit ObjectEnvelopes (that is object-classifier’s job)

## Build

```bash
# Needs: CMake ≥ 3.20, C++20, OpenCV ≥ 5, protobuf, gRPC
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

## Run

### Local demo (no network)

```bash
./build/camera_connector 0                    # webcam
./build/camera_connector /dev/video0 1280 720
./build/camera_connector rtsp://user:pass@cam/stream
./build/camera_connector /path/to/video.mp4
```

### Network path (`--publish`)

Starts a gRPC `FrameService` server and streams `FrameEnvelope` to subscribers
(object-classifier `--network-path`).

```bash
# Default listen 0.0.0.0:50060, JPEG quality 80
./build/camera_connector --publish 0
./build/camera_connector --publish /path/to/video.mp4

# Env overrides
FRAME_GRPC_ADDR=0.0.0.0:50060 \
FRAME_ENCODING=jpeg \
FRAME_JPEG_QUALITY=80 \
./build/camera_connector --publish rtsp://cam/stream
```

| Env | Default | Meaning |
|-----|---------|---------|
| `FRAME_GRPC_ADDR` | `0.0.0.0:50060` | gRPC listen address |
| `FRAME_ENCODING` | `jpeg` | `jpeg` or `raw_bgr` |
| `FRAME_JPEG_QUALITY` | `80` | JPEG quality 1–100 |

### Contract

```
camera-connector --publish  →  gRPC FrameService.Subscribe (stream FrameEnvelope)
                                      ↓
                            object-classifier --network-path
```

Proto: `proto/capture/v1/frame.proto` + `frame_service.proto`.

## Integration

| Mode | Contract toward object-classifier |
|------|-----------------------------------|
| `--in-process` (classifier side) | Shared `camera::FrameQueue` / vendored capture |
| `--publish` / `--network-path` | gRPC `FrameEnvelope` stream on `FRAME_GRPC_ADDR` |

Future: ONVIF discovery, proprietary SDKs, zero-copy (`payload_ref` / NVMM).
