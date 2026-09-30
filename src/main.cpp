// camera-connector
// Thin multi-source capture that feeds a FrameQueue and optionally publishes
// FrameEnvelope streams over gRPC (network path for object-classifier).
//
// Modes:
//   local (default)  – capture → FrameQueue → log sink (unit / smoke test)
//   --publish        – capture → FrameHub → gRPC FrameService (network path)

#include "capture.hpp"
#include "frame_publisher.hpp"

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

void print_usage(const char* argv0) {
  std::cerr
      << "Usage:\n"
      << "  " << argv0 << " <source> [target_w] [target_h]\n"
      << "  " << argv0 << " --publish <source> [target_w] [target_h]\n"
      << "\n"
      << "  source     0 | /dev/video0 | rtsp://... | file.mp4\n"
      << "  --publish  start gRPC FrameService and stream FrameEnvelope\n"
      << "\n"
      << "Environment:\n"
      << "  FRAME_GRPC_ADDR   listen address (default 0.0.0.0:50060)\n"
      << "  FRAME_ENCODING    jpeg | raw_bgr (default jpeg)\n"
      << "  FRAME_JPEG_QUALITY 1-100 (default 80)\n";
}
}  // namespace

int main(int argc, char** argv) {
  spdlog::set_level(spdlog::level::info);

  if (argc < 2) {
    print_usage(argv[0]);
    return 1;
  }

  bool publish = false;
  int argi = 1;
  if (std::string(argv[1]) == "--publish") {
    publish = true;
    ++argi;
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

  if (!publish) {
    // ── Local demo sink ───────────────────────────────────────────────────
    spdlog::info("camera-connector [local] source={} – demo sink",
                 cap_cfg.source);
    while (g_running && capture.is_running()) {
      auto item = queue->pop(std::chrono::milliseconds(500));
      if (!item) continue;
      auto& [frame, meta] = *item;
      if (meta.frame_id % 30 == 0) {
        spdlog::info("frame {}  {}x{}  source={}", meta.frame_id, meta.width,
                     meta.height, meta.source);
      }
    }
    capture.stop();
    spdlog::info("camera-connector stopped");
    return 0;
  }

  // ── Network path: gRPC FrameService ─────────────────────────────────────
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
    capture.stop();
    return 1;
  }

  spdlog::info(
      "camera-connector [publish] source={} addr={} encoding={} jpeg_q={}",
      cap_cfg.source, pub_cfg.listen_addr, pub_cfg.default_encoding,
      pub_cfg.jpeg_quality);

  while (g_running && capture.is_running()) {
    auto item = queue->pop(std::chrono::milliseconds(500));
    if (!item) continue;
    auto& [frame, meta] = *item;
    hub->publish(frame, meta);
    if (meta.frame_id % 30 == 0) {
      spdlog::info("published frame {}  {}x{}  subscribers={}",
                   meta.frame_id, meta.width, meta.height,
                   hub->subscriber_count());
    }
  }

  server.stop();
  capture.stop();
  spdlog::info("camera-connector [publish] stopped");
  return 0;
}
