#pragma once

#include "types.hpp"

#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace camera {

// Rolling-segment video recorder that uploads completed MP4 segments to
// agentjetson/core object-storage (HTTP POST /v1/objects?kind=camera_recording).
// The object lands in the configured S3-compatible backend (RustFS in core compose).
class IngestSink {
public:
  struct Config {
    std::string object_storage_http{"http://127.0.0.1:8081"};
    std::string source;                 // camera_id / source label for object_meta
    int         segment_sec{30};        // roll a new file every N seconds
    std::string tmp_dir{"/tmp/camera-connector-ingest"};
    int         fps{15};                // nominal fps for VideoWriter
    int         width{0};               // set from first frame if 0
    int         height{0};
  };

  explicit IngestSink(Config cfg);
  ~IngestSink();

  // Feed a frame. Thread-safe; may block briefly when rolling a segment.
  void push(const cv::Mat& frame, const FrameMeta& meta);

  // Flush current segment and stop accepting frames.
  void stop();

  bool is_running() const { return running_.load(); }
  int64_t segments_uploaded() const { return segments_uploaded_.load(); }

private:
  void open_writer(int width, int height);
  void close_and_upload();
  bool upload_file(const std::filesystem::path& path, const std::string& content_type);

  Config cfg_;
  std::mutex mtx_;
  std::unique_ptr<cv::VideoWriter> writer_;
  std::filesystem::path current_path_;
  std::chrono::steady_clock::time_point segment_start_;
  std::atomic<bool> running_{true};
  std::atomic<int64_t> segments_uploaded_{0};
  int width_{0};
  int height_{0};
};

}  // namespace camera
