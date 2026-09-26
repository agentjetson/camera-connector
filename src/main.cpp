// camera-connector skeleton
// Thin multi-source capture that feeds a FrameQueue (or later a gRPC/NATS publisher).
// Industry camera support can be extended with ONVIF / proprietary SDKs.

#include "capture.hpp"

#include <atomic>
#include <csignal>
#include <iostream>
#include <thread>

#include <spdlog/spdlog.h>

namespace {
std::atomic<bool> g_running{true};
void on_signal(int) { g_running = false; }
}  // namespace

int main(int argc, char** argv) {
  spdlog::set_level(spdlog::level::info);

  if (argc < 2) {
    std::cerr << "Usage: " << argv[0]
              << " <source (0|/dev/video0|rtsp://...|file.mp4)> [target_w] [target_h]\n";
    return 1;
  }

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  camera::Capture::Config cfg;
  cfg.source = argv[1];
  if (argc > 2) cfg.target_width  = std::stoi(argv[2]);
  if (argc > 3) cfg.target_height = std::stoi(argv[3]);

  auto queue = std::make_shared<camera::FrameQueue>(2);
  camera::Capture capture(cfg, queue);
  capture.start();

  spdlog::info("camera-connector running – consuming frames from queue (demo sink)");
  while (g_running && capture.is_running()) {
    auto item = queue->pop(std::chrono::milliseconds(500));
    if (!item) continue;
    auto& [frame, meta] = *item;
    // Demo: just log. Real integration hands the frame to object-classifier
    // via shared memory, gRPC stream, or in-process FrameSink.
    if (meta.frame_id % 30 == 0) {
      spdlog::info("frame {}  {}x{}  source={}", meta.frame_id, meta.width,
                   meta.height, meta.source);
    }
  }

  capture.stop();
  spdlog::info("camera-connector stopped");
  return 0;
}
