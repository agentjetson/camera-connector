# syntax=docker/dockerfile:1.7
# camera-connector — thin multi-source capture + optional gRPC FrameService publish
# Base: agentjetsonai/cv-base:ubuntu26.04-opencv5.0.0-ort1.30.0

FROM agentjetsonai/cv-base AS builder

ENV DEBIAN_FRONTEND=noninteractive \
    GIT_TERMINAL_PROMPT=0

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake git pkg-config curl ca-certificates \
        libabsl-dev \
        libprotobuf-dev protobuf-compiler \
        libgrpc++-dev protobuf-compiler-grpc \
        libspdlog-dev \
    && update-ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN rm -rf build CMakeCache.txt CMakeFiles \
    && git config --global http.version HTTP/1.1 \
    && cmake -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j$(nproc) --target camera_connector

FROM agentjetsonai/cv-base

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        libprotobuf32t64 \
        libgrpc++1.51t64 \
        libssl3t64 \
        libabsl20260107 \
        libspdlog1.15 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /src/build/camera_connector /app/camera_connector

ENV LD_LIBRARY_PATH=/opt/opencv/lib \
    FRAME_GRPC_ADDR=0.0.0.0:50060 \
    FRAME_ENCODING=jpeg \
    FRAME_JPEG_QUALITY=80

EXPOSE 50060
WORKDIR /app

# Default: network publish of webcam index 0. Override source via command.
CMD ["/app/camera_connector", "--publish", "0"]
