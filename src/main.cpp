// camera-connector
// Thin multi-source capture that feeds a FrameQueue and optionally publishes
// FrameEnvelope streams over gRPC (network path for object-classifier) and/or
// persists rolling video segments into core object-storage (RustFS).
//
// Modes:
//   local (default)  – capture → FrameQueue → log sink (unit / smoke test)
//   --publish        – capture → FrameHub → gRPC FrameService (network path)
//   --ingest         – capture → rolling MP4 segments → object-storage HTTP
//   flags may be combined: --publish --ingest <source> …

#include "capture.hpp"
#include "frame_publisher.hpp"
#include "ingest.hpp"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include <spdlog/spdlog.h>

namespace {
std::atomic<bool> g_running{true};
void on_signal(int) { g_running = false; }

std::string env_or(const char* key, const std::string& fallback) {
  if (const char* v = std::getenv(key); v && *v) return v;
  return fallback;
}

bool env_truthy(const char* key) {
  const char* v = std::getenv(key);
  if (!v || !*v) return false;
  std::string s(v);
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s == "1" || s == "true" || s == "yes" || s == "on";
}

void print_usage(const char* argv0) {
  std::cerr
      << "Usage:\n"
      << "  " << argv0 << " [flags] <source> [target_w] [target_h]\n"
      << "\n"
      << "Flags (order-independent, before <source>):\n"
      << "  --publish   start gRPC FrameService and stream FrameEnvelope\n"
      << "  --ingest    record rolling MP4 segments and POST to object-storage\n"
      << "\n"
      << "  source     0 | /dev/video0 | rtsp://... | file.mp4\n"
      << "\n"
      << "Environment:\n"
      << "  FRAME_GRPC_ADDR        listen address (default 0.0.0.0:50060)\n"
      << "  FRAME_ENCODING         jpeg | raw_bgr (default jpeg)\n"
      << "  FRAME_JPEG_QUALITY     1-100 (default 80)\n"
      << "  INGEST                 true|1|yes to enable ingest without --ingest\n"
      << "  OBJECT_STORAGE_HTTP    object-storage base URL (default http://127.0.0.1:8081)\n"
      << "  INGEST_SEGMENT_SEC     segment duration seconds (default 30)\n"
      << "  INGEST_SOURCE          source label for object_meta (default = capture source)\n"
      << "  INGEST_TMP_DIR         temp dir for segments (default /tmp/camera-connector-ingest)\n";
}
}  // namespace

int main(int argc, char** argv) {
  spdlog::set_level(spdlog::level::info);

  if (argc < 2) {
    print_usage(argv[0]);
    return 1;
  }

  bool publish = false;
  bool ingest = env_truthy("INGEST");
  int argi = 1;
  while (argi < argc) {
    std::string a = argv[argi];
    if (a == "--publish") {
      publish = true;
      ++argi;
    } else if (a == "--ingest") {
      ingest = true;
      ++argi;
    } else if (a == "-h" || a == "--help") {
      print_usage(argv[0]);
      return 0;
    } else {
      break;
    }
  }
  if (argi >= argc) {
    print_usage(argv[0]);
    return 1;
  }

  camera::Capture::Config cap_cfg;
  cap_cfg.source = argv[argi++];
  if (argi < argc) cap_cfg.target_width = std::stoi(argv[argi++]);
  if (argi < argc) cap_cfg.target_height = std::stoi(argv[argi++]);

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  auto queue = std::make_shared<camera::FrameQueue>(2);
  camera::Capture capture(cap_cfg, queue);
  capture.start();

  // Optional ingest sink
  std::unique_ptr<camera::IngestSink> ingest_sink;
  if (ingest) {
    camera::IngestSink::Config icfg;
    icfg.object_storage_http =
        env_or("OBJECT_STORAGE_HTTP", "http://127.0.0.1:8081");
    icfg.source = env_or("INGEST_SOURCE", cap_cfg.source);
    icfg.segment_sec = std::stoi(env_or("INGEST_SEGMENT_SEC", "30"));
    icfg.tmp_dir = env_or("INGEST_TMP_DIR", "/tmp/camera-connector-ingest");
    if (cap_cfg.target_width > 0) icfg.width = cap_cfg.target_width;
    if (cap_cfg.target_height > 0) icfg.height = cap_cfg.target_height;
    ingest_sink = std::make_unique<camera::IngestSink>(std::move(icfg));
  }

  if (!publish) {
    // ── Local demo (+ optional ingest) ────────────────────────────────────
    spdlog::info("camera-connector [local{}] source={}",
                 ingest ? "+ingest" : "", cap_cfg.source);
    while (g_running && capture.is_running()) {
      auto item = queue->pop(std::chrono::milliseconds(500));
      if (!item) continue;
      auto& [frame, meta] = *item;
      if (ingest_sink) ingest_sink->push(frame, meta);
      if (meta.frame_id % 30 == 0) {
        spdlog::info("frame {}  {}x{}  source={}", meta.frame_id, meta.width,
                     meta.height, meta.source);
      }
    }
    if (ingest_sink) ingest_sink->stop();
    capture.stop();
    spdlog::info("camera-connector stopped");
    return 0;
  }

  // ── Network path: gRPC FrameService (+ optional ingest) ─────────────────
  auto hub = std::make_shared<camera::FrameHub>();

  camera::FramePublisherServer::Config pub_cfg;
  pub_cfg.listen_addr =
      env_or("FRAME_GRPC_ADDR", "0.0.0.0:50060");
  pub_cfg.default_encoding =
      env_or("FRAME_ENCODING", "jpeg");
  pub_cfg.jpeg_quality =
      std::stoi(env_or("FRAME_JPEG_QUALITY", "80"));

  camera::FramePublisherServer server(pub_cfg, hub);
  server.start();
  if (!server.is_running()) {
    spdlog::error("failed to start FramePublisherServer");
    if (ingest_sink) ingest_sink->stop();
    capture.stop();
    return 1;
  }

  spdlog::info(
      "camera-connector [publish{}] source={} addr={} encoding={} jpeg_q={}",
      ingest ? "+ingest" : "", cap_cfg.source, pub_cfg.listen_addr,
      pub_cfg.default_encoding, pub_cfg.jpeg_quality);

  while (g_running && capture.is_running()) {
    auto item = queue->pop(std::chrono::milliseconds(500));
    if (!item) continue;
    auto& [frame, meta] = *item;
    hub->publish(frame, meta);
    if (ingest_sink) ingest_sink->push(frame, meta);
    if (meta.frame_id % 30 == 0) {
      spdlog::info("published frame {}  {}x{}  subscribers={}",
                   meta.frame_id, meta.width, meta.height,
                   hub->subscriber_count());
    }
  }

  if (ingest_sink) ingest_sink->stop();
  server.stop();
  capture.stop();
  spdlog::info("camera-connector [publish] stopped");
  return 0;
}
