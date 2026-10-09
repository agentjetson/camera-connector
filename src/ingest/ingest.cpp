#include "ingest.hpp"

#include <opencv2/imgproc.hpp>
#include <spdlog/spdlog.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

namespace camera {
namespace fs = std::filesystem;

IngestSink::IngestSink(Config cfg) : cfg_(std::move(cfg)) {
  if (cfg_.source.empty()) cfg_.source = "camera";
  if (cfg_.segment_sec <= 0) cfg_.segment_sec = 30;
  if (cfg_.fps <= 0) cfg_.fps = 15;
  std::error_code ec;
  fs::create_directories(cfg_.tmp_dir, ec);
  if (ec) {
    spdlog::warn("ingest: could not create tmp_dir {}: {}", cfg_.tmp_dir, ec.message());
  }
  spdlog::info("ingest: enabled source={} segment_sec={} object_storage={}",
               cfg_.source, cfg_.segment_sec, cfg_.object_storage_http);
}

IngestSink::~IngestSink() { stop(); }

void IngestSink::push(const cv::Mat& frame, const FrameMeta& /*meta*/) {
  if (!running_.load() || frame.empty()) return;

  std::lock_guard<std::mutex> lock(mtx_);
  if (!running_.load()) return;

  if (!writer_) {
    width_ = cfg_.width > 0 ? cfg_.width : frame.cols;
    height_ = cfg_.height > 0 ? cfg_.height : frame.rows;
    open_writer(width_, height_);
    if (!writer_) return;
  }

  cv::Mat to_write = frame;
  if (frame.cols != width_ || frame.rows != height_) {
    cv::Mat resized;
    cv::resize(frame, resized, cv::Size(width_, height_));
    to_write = resized;
  }
  writer_->write(to_write);

  const auto now = std::chrono::steady_clock::now();
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::seconds>(now - segment_start_).count();
  if (elapsed >= cfg_.segment_sec) {
    close_and_upload();
    open_writer(width_, height_);
  }
}

void IngestSink::stop() {
  if (!running_.exchange(false)) return;
  std::lock_guard<std::mutex> lock(mtx_);
  close_and_upload();
  spdlog::info("ingest: stopped (segments_uploaded={})", segments_uploaded_.load());
}

void IngestSink::open_writer(int width, int height) {
  const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
  std::ostringstream name;
  name << "seg_" << cfg_.source << "_" << stamp << ".mp4";
  // sanitise source for filesystem
  std::string safe = name.str();
  for (char& c : safe) {
    if (c == '/' || c == '\\' || c == ':') c = '_';
  }
  current_path_ = fs::path(cfg_.tmp_dir) / safe;

  // Prefer mp4v; fall back to XVID / MJPG if unavailable on the host.
  int fourcc = cv::VideoWriter::fourcc('m', 'p', '4', 'v');
  writer_ = std::make_unique<cv::VideoWriter>(
      current_path_.string(), fourcc, static_cast<double>(cfg_.fps),
      cv::Size(width, height), true);
  if (!writer_->isOpened()) {
    fourcc = cv::VideoWriter::fourcc('X', 'V', 'I', 'D');
    writer_ = std::make_unique<cv::VideoWriter>(
        current_path_.string(), fourcc, static_cast<double>(cfg_.fps),
        cv::Size(width, height), true);
  }
  if (!writer_->isOpened()) {
    fourcc = cv::VideoWriter::fourcc('M', 'J', 'P', 'G');
    current_path_.replace_extension(".avi");
    writer_ = std::make_unique<cv::VideoWriter>(
        current_path_.string(), fourcc, static_cast<double>(cfg_.fps),
        cv::Size(width, height), true);
  }
  if (!writer_->isOpened()) {
    spdlog::error("ingest: failed to open VideoWriter for {}", current_path_.string());
    writer_.reset();
    return;
  }
  segment_start_ = std::chrono::steady_clock::now();
  spdlog::debug("ingest: opened segment {}", current_path_.string());
}

void IngestSink::close_and_upload() {
  if (!writer_) return;
  writer_->release();
  writer_.reset();

  if (current_path_.empty() || !fs::exists(current_path_)) return;

  const auto size = fs::file_size(current_path_);
  if (size == 0) {
    spdlog::warn("ingest: empty segment {}, skipping upload", current_path_.string());
    fs::remove(current_path_);
    current_path_.clear();
    return;
  }

  std::string content_type = "video/mp4";
  if (current_path_.extension() == ".avi") content_type = "video/x-msvideo";

  spdlog::info("ingest: uploading {} ({} bytes) → {}", current_path_.string(), size,
               cfg_.object_storage_http);
  if (upload_file(current_path_, content_type)) {
    segments_uploaded_++;
    fs::remove(current_path_);
  } else {
    spdlog::error("ingest: upload failed; leaving file at {}", current_path_.string());
  }
  current_path_.clear();
}

bool IngestSink::upload_file(const fs::path& path, const std::string& content_type) {
  // Use system curl for a minimal dependency surface (available on Jetson / Ubuntu images).
  // POST /v1/objects?kind=camera_recording&source=<source>
  std::ostringstream cmd;
  cmd << "curl -sS -f -X POST "
      << "'" << cfg_.object_storage_http << "/v1/objects"
      << "?kind=camera_recording&source=" << cfg_.source << "' "
      << "-H 'Content-Type: " << content_type << "' "
      << "--data-binary @'" << path.string() << "' "
      << "-o /tmp/camera-connector-ingest-last.json "
      << "-w '%{http_code}'";

  FILE* pipe = popen(cmd.str().c_str(), "r");
  if (!pipe) {
    spdlog::error("ingest: popen(curl) failed");
    return false;
  }
  char buf[32] = {};
  std::string http_code;
  while (fgets(buf, sizeof(buf), pipe)) http_code += buf;
  const int rc = pclose(pipe);
  // strip trailing newline
  while (!http_code.empty() &&
         (http_code.back() == '\n' || http_code.back() == '\r'))
    http_code.pop_back();

  if (rc != 0 || http_code.empty() || http_code[0] != '2') {
    spdlog::error("ingest: curl exit={} http_code={} (see /tmp/camera-connector-ingest-last.json)",
                  rc, http_code);
    return false;
  }
  spdlog::info("ingest: upload ok http={} response in /tmp/camera-connector-ingest-last.json",
               http_code);
  return true;
}

}  // namespace camera
