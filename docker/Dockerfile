# syntax=docker/dockerfile:1.7
# camera-connector — thin multi-source capture (V4L2 / RTSP / file) → FrameQueue
# Base: agentjetsonai/cv-base:ubuntu26.04-opencv5.0.0-ort1.30.0
#   (Ubuntu 26.04 + OpenCV 5.0.0 + ORT 1.30.0 @ /opt/*)

FROM agentjetsonai/cv-base AS builder

ENV DEBIAN_FRONTEND=noninteractive \
    GIT_TERMINAL_PROMPT=0

# Build deps only — OpenCV already lives in the base
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake git pkg-config curl ca-certificates \
        libspdlog-dev \
    && update-ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

# Base already exports OpenCV_DIR
RUN rm -rf build CMakeCache.txt CMakeFiles \
    && git config --global http.version HTTP/1.1 \
    && cmake -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j$(nproc) --target camera_connector

# ── runtime ──────────────────────────────────────────────
FROM agentjetsonai/cv-base

ENV DEBIAN_FRONTEND=noninteractive

# Shared libs the binary links against (OpenCV comes from the base)
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        libspdlog1.15 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /src/build/camera_connector /app/camera_connector

ENV LD_LIBRARY_PATH=/opt/opencv/lib

WORKDIR /app

# Default: open webcam index 0. Override via command or compose.
# For host cameras: --device /dev/video0  (or privileged + devices)
# For RTSP / file: pass the URL or path as the first argument.
CMD ["/app/camera_connector", "0"]