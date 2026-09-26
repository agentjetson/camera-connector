# camera-connector

Thin, device-local camera capture for AgentJetson.

**Responsibilities**
- Open industry camera sources: V4L2 (`/dev/video*`), RTSP, file, USB UVC
- Normalize frames (optional resize)
- Feed a low-latency FrameQueue (drop-oldest)
- Future: ONVIF discovery, proprietary SDKs, zero-copy (NVMM/DMA-BUF), health monitoring

**Does not**
- Run any ML models
- Emit ObjectEnvelopes (that is object-classifier’s job)

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

## Run

```bash
./build/camera_connector 0                    # webcam
./build/camera_connector /dev/video0 1280 720
./build/camera_connector rtsp://user:pass@cam/stream
./build/camera_connector /path/to/video.mp4
```

## Integration

The `camera::FrameQueue` (or a `FrameSink` callback) is the contract toward
`object-classifier`. Later a gRPC / NATS FrameEnvelope publisher can be added
using `proto/capture/v1/frame.proto`.
