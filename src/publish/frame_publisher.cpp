#include "frame_publisher.hpp"

#include <opencv2/imgcodecs.hpp>
#include <grpcpp/grpcpp.h>
#include <google/protobuf/util/time_util.h>
#include <spdlog/spdlog.h>

#include "capture/v1/frame.pb.h"
#include "capture/v1/frame_service.grpc.pb.h"

#include <algorithm>
#include <chrono>
#include <deque>

namespace camera {

// ── FrameHub ────────────────────────────────────────────────────────────────

size_t FrameHub::subscribe(Subscriber cb) {
  std::lock_guard<std::mutex> lock(mtx_);
  size_t id = next_id_++;
  subs_.emplace_back(id, std::move(cb));
  return id;
}

void FrameHub::unsubscribe(size_t id) {
  std::lock_guard<std::mutex> lock(mtx_);
  subs_.erase(
      std::remove_if(subs_.begin(), subs_.end(),
                     [id](const auto& p) { return p.first == id; }),
      subs_.end());
}

void FrameHub::publish(const cv::Mat& frame, const FrameMeta& meta) {
  std::vector<size_t> dead;
  {
    std::lock_guard<std::mutex> lock(mtx_);
    for (auto& [id, cb] : subs_) {
      try {
        if (!cb(frame, meta)) {
          dead.push_back(id);
        }
      } catch (const std::exception& e) {
        spdlog::warn("FrameHub subscriber {}: {}", id, e.what());
        dead.push_back(id);
      }
    }
  }
  for (size_t id : dead) {
    unsubscribe(id);
  }
}

size_t FrameHub::subscriber_count() const {
  std::lock_guard<std::mutex> lock(mtx_);
  return subs_.size();
}

// ── gRPC service impl ───────────────────────────────────────────────────────

namespace {

capture::v1::FrameEnvelope encode_frame(
    const cv::Mat& frame,
    const FrameMeta& meta,
    const std::string& encoding,
    int jpeg_quality) {
  capture::v1::FrameEnvelope env;
  env.set_frame_id(meta.frame_id);
  *env.mutable_timestamp() = google::protobuf::util::TimeUtil::GetCurrentTime();
  env.set_source(meta.source);
  env.set_width(meta.width > 0 ? meta.width : frame.cols);
  env.set_height(meta.height > 0 ? meta.height : frame.rows);
  for (const auto& [k, v] : meta.labels) {
    (*env.mutable_labels())[k] = v;
  }

  auto now = Clock::now();
  env.set_capture_latency_ms(DurationMs(now - meta.capture_ts).count());

  if (encoding == "raw_bgr" && frame.isContinuous() && frame.type() == CV_8UC3) {
    env.set_encoding("raw_bgr");
    env.set_payload(frame.data, static_cast<size_t>(frame.total() * frame.elemSize()));
  } else {
    // Default / fallback: JPEG
    std::vector<uchar> buf;
    std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, jpeg_quality};
    if (cv::imencode(".jpg", frame, buf, params)) {
      env.set_encoding("jpeg");
      env.set_payload(buf.data(), buf.size());
    } else {
      spdlog::warn("JPEG encode failed for frame {}", meta.frame_id);
    }
  }
  return env;
}

class FrameServiceImpl final : public capture::v1::FrameService::Service {
public:
  FrameServiceImpl(std::shared_ptr<FrameHub> hub, int jpeg_quality,
                   std::string default_encoding)
      : hub_(std::move(hub)),
        jpeg_quality_(jpeg_quality),
        default_encoding_(std::move(default_encoding)) {}

  grpc::Status Subscribe(
      grpc::ServerContext* context,
      const capture::v1::SubscribeRequest* request,
      grpc::ServerWriter<capture::v1::FrameEnvelope>* writer) override {
    const std::string encoding =
        request->preferred_encoding().empty()
            ? default_encoding_
            : request->preferred_encoding();
    const std::string camera_filter = request->camera_id();

    spdlog::info("FrameService: client subscribed (filter='{}' encoding={})",
                 camera_filter, encoding);

    // Bounded queue between hub callback and the write loop so we never
    // block the capture thread on a slow client.
    struct Slot {
      cv::Mat frame;
      FrameMeta meta;
    };
    auto q = std::make_shared<std::deque<Slot>>();
    auto q_mtx = std::make_shared<std::mutex>();
    auto q_cv = std::make_shared<std::condition_variable>();
    auto done = std::make_shared<std::atomic<bool>>(false);
    constexpr size_t kMaxQueue = 2;  // drop-oldest under backpressure

    size_t sub_id = hub_->subscribe(
        [q, q_mtx, q_cv, done, camera_filter](const cv::Mat& frame,
                                              const FrameMeta& meta) -> bool {
          if (done->load()) return false;
          if (!camera_filter.empty()) {
            bool match = (meta.source == camera_filter);
            if (!match) {
              auto it = meta.labels.find("camera_id");
              if (it != meta.labels.end() && it->second == camera_filter)
                match = true;
            }
            if (!match) return true;  // filtered out, stay subscribed
          }
          {
            std::lock_guard<std::mutex> lock(*q_mtx);
            while (q->size() >= kMaxQueue) q->pop_front();
            q->push_back(Slot{frame.clone(), meta});
          }
          q_cv->notify_one();
          return true;
        });

    while (!context->IsCancelled() && !done->load()) {
      Slot slot;
      {
        std::unique_lock<std::mutex> lock(*q_mtx);
        q_cv->wait_for(lock, std::chrono::milliseconds(500),
                       [&] { return !q->empty() || done->load() ||
                                    context->IsCancelled(); });
        if (q->empty()) continue;
        slot = std::move(q->front());
        q->pop_front();
      }

      auto env = encode_frame(slot.frame, slot.meta, encoding, jpeg_quality_);
      if (!writer->Write(env)) {
        spdlog::info("FrameService: write failed / client gone");
        break;
      }
    }

    done->store(true);
    hub_->unsubscribe(sub_id);
    spdlog::info("FrameService: client disconnected");
    return grpc::Status::OK;
  }

private:
  std::shared_ptr<FrameHub> hub_;
  int jpeg_quality_;
  std::string default_encoding_;
};

}  // namespace

// ── FramePublisherServer ────────────────────────────────────────────────────

struct FramePublisherServer::Impl {
  std::unique_ptr<grpc::Server> server;
  std::unique_ptr<FrameServiceImpl> service;
};

FramePublisherServer::FramePublisherServer(Config cfg,
                                           std::shared_ptr<FrameHub> hub)
    : cfg_(std::move(cfg)), hub_(std::move(hub)), impl_(std::make_unique<Impl>()) {}

FramePublisherServer::~FramePublisherServer() { stop(); }

void FramePublisherServer::start() {
  if (running_) return;

  impl_->service = std::make_unique<FrameServiceImpl>(
      hub_, cfg_.jpeg_quality, cfg_.default_encoding);

  grpc::ServerBuilder builder;
  builder.AddListeningPort(cfg_.listen_addr, grpc::InsecureServerCredentials());
  builder.RegisterService(impl_->service.get());
  // Large messages for raw frames / high-res JPEG
  builder.SetMaxSendMessageSize(32 * 1024 * 1024);
  builder.SetMaxReceiveMessageSize(32 * 1024 * 1024);

  impl_->server = builder.BuildAndStart();
  if (!impl_->server) {
    spdlog::error("FramePublisherServer: failed to bind {}", cfg_.listen_addr);
    return;
  }

  running_ = true;
  spdlog::info("FramePublisherServer listening on {}", cfg_.listen_addr);

  server_thread_ = std::make_unique<std::thread>([this] {
    impl_->server->Wait();
    running_ = false;
  });
}

void FramePublisherServer::stop() {
  if (impl_ && impl_->server) {
    impl_->server->Shutdown(std::chrono::system_clock::now() +
                            std::chrono::milliseconds(500));
  }
  if (server_thread_ && server_thread_->joinable()) {
    server_thread_->join();
  }
  server_thread_.reset();
  running_ = false;
}

}  // namespace camera
