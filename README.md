# camera-connector

Thin, device-local camera capture for AgentJetson.

**Responsibilities**
- Open industry camera sources: V4L2 (`/dev/video*`), RTSP, file, USB UVC
- Normalize frames (optional resize)
- Feed a low-latency FrameQueue (drop-oldest)
- **Network path:** publish `FrameEnvelope` streams over gRPC (`FrameService`)
- **Ingest path:** record rolling MP4 segments and persist them into
  `agentjetson/core` object-storage (RustFS bucket)

**Does not**
- Run any ML models
- Emit ObjectEnvelopes (that is object-classifier’s job)

## Build

```bash
# Needs: CMake ≥ 3.20, C++20, OpenCV ≥ 5, protobuf, gRPC, curl (for --ingest upload)
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

### Ingest path (`--ingest` / `INGEST=true`)

Records rolling video segments and POSTs each completed segment to core
**object-storage**, which writes the blob into the RustFS (S3-compatible)
bucket and indexes it in ClickHouse `object_meta`.

```bash
# 30s segments (default) → http://127.0.0.1:8081
./build/camera_connector --ingest 0

# Combine with publish
./build/camera_connector --publish --ingest rtsp://cam/stream

# Env-driven
INGEST=true \
OBJECT_STORAGE_HTTP=http://127.0.0.1:8081 \
INGEST_SEGMENT_SEC=60 \
INGEST_SOURCE=cam-front \
./build/camera_connector /dev/video0
```

| Env | Default | Meaning |
|-----|---------|---------|
| `INGEST` | (unset) | `true`/`1`/`yes` enables ingest without `--ingest` |
| `OBJECT_STORAGE_HTTP` | `http://127.0.0.1:8081` | Base URL of core object-storage HTTP surface |
| `INGEST_SEGMENT_SEC` | `30` | Seconds per MP4 segment before roll + upload |
| `INGEST_SOURCE` | capture source | Label written into object_meta.source |
| `INGEST_TMP_DIR` | `/tmp/camera-connector-ingest` | Local staging directory for segments |

Upload contract:

```
POST {OBJECT_STORAGE_HTTP}/v1/objects?kind=camera_recording&source={INGEST_SOURCE}
Content-Type: video/mp4
Body: <segment bytes>
```

Successful puts land under:

```
camera_recording/<source>/YYYY/MM/DD/<uuid>.mp4
```

in the RustFS bucket configured by object-storage (`agentjetson` by default).

### Contract

```
camera-connector --publish  →  gRPC FrameService.Subscribe (stream FrameEnvelope)
                                      ↓
                            object-classifier --network-path

camera-connector --ingest   →  HTTP POST object-storage /v1/objects
                                      ↓
                            RustFS bucket + object_meta (ClickHouse)
```

Proto: `proto/capture/v1/frame.proto` + `frame_service.proto` (from agentjetson/core).
Storage kind: `OBJECT_KIND_CAMERA_RECORDING` in `proto/storage/v1/object_storage.proto`.

## Integration

| Mode | Contract toward downstream |
|------|----------------------------|
| `--in-process` (classifier side) | Shared `camera::FrameQueue` / vendored capture |
| `--publish` / `--network-path` | gRPC `FrameEnvelope` stream on `FRAME_GRPC_ADDR` |
| `--ingest` | HTTP put of `camera_recording` blobs into core object-storage → RustFS |

Future: ONVIF discovery, proprietary SDKs, zero-copy (`payload_ref` / NVMM),
native gRPC ObjectStorageService client once core wires generated stubs.
