# syntax=docker/dockerfile:1.7
FROM agentjetsonai/gecko AS builder

ENV DEBIAN_FRONTEND=noninteractive \
    GIT_TERMINAL_PROMPT=0

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake git \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN rm -rf build CMakeCache.txt CMakeFiles \
    && git config --global http.version HTTP/1.1 \
    && cmake -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j"$(nproc)" --target camera_connector

FROM agentjetsonai/gecko
ENV DEBIAN_FRONTEND=noninteractive

# curl is required for --ingest HTTP uploads to object-storage
RUN apt-get update && apt-get install -y --no-install-recommends \
        curl \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /src/build/camera_connector /app/camera_connector

ENV LD_LIBRARY_PATH=/opt/opencv/lib \
    FRAME_GRPC_ADDR=0.0.0.0:50060 \
    FRAME_ENCODING=jpeg \
    FRAME_JPEG_QUALITY=80

EXPOSE 50060
WORKDIR /app
CMD ["/app/camera_connector", "--publish", "0"]
