#pragma once

#include "capture.hpp"
#include "types.hpp"

#include <opencv2/core.hpp>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <functional>

namespace camera {

// Fan-out hub: capture pushes frames here; each gRPC subscriber gets a copy.
class FrameHub {
public:
  using Subscriber = std::function<bool(const cv::Mat&, const FrameMeta&)>;

  // Register a subscriber. Returns a handle used to unregister.
  // Subscriber returns false to signal it wants to stop receiving.
  size_t subscribe(Subscriber cb);
  void unsubscribe(size_t id);

  // Called by the capture loop (or main) for every new frame.
  void publish(const cv::Mat& frame, const FrameMeta& meta);

  size_t subscriber_count() const;

private:
  mutable std::mutex mtx_;
  size_t next_id_{1};
  std::vector<std::pair<size_t, Subscriber>> subs_;
};

// gRPC FrameService server that streams FrameEnvelope to subscribers.
class FramePublisherServer {
public:
  struct Config {
    std::string listen_addr{"0.0.0.0:50060"};
    int         jpeg_quality{80};
    std::string default_encoding{"jpeg"};  // "jpeg" | "raw_bgr"
  };

  FramePublisherServer(Config cfg, std::shared_ptr<FrameHub> hub);
  ~FramePublisherServer();

  void start();
  void stop();
  bool is_running() const { return running_.load(); }

private:
  Config cfg_;
  std::shared_ptr<FrameHub> hub_;
  std::unique_ptr<std::thread> server_thread_;
  std::atomic<bool> running_{false};
  // Opaque: holds grpc::Server so we don't leak grpc headers into every TU.
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace camera
