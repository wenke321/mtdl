#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "mtdl/core/endpoint_pool.hpp"
#include "mtdl/core/file_sink.hpp"
#include "mtdl/core/progress.hpp"
#include "mtdl/core/segment.hpp"
#include "mtdl/core/state.hpp"
#include "mtdl/net/http/client.hpp"

namespace mtdl {

struct WorkerContext {
  Url url;
  ClientOptions client;
  uint32_t max_retries = 6;
  uint64_t total_size = 0;
};

// One worker == one TCP connection pinned to one edge address. It pulls chunks
// from the shared WorkQueue and streams them back-to-back, reusing the
// connection (keep-alive) until the queue is empty. When its edge turns out to
// be unusable the worker leases another edge and carries on; when it has to
// give up entirely it hands the unread tail back to the queue.
class Worker {
 public:
  Worker(uint32_t id, const WorkerContext& ctx, WorkQueue& queue, FileSink& sink,
         Progress& progress, DownloadState& state, std::atomic<bool>& cancel,
         EndpointPool* pool = nullptr);

  void run();
  uint64_t bytes() const { return bytes_.load(std::memory_order_relaxed); }
  uint32_t errors() const { return errors_.load(std::memory_order_relaxed); }
  const std::string& last_error() const { return last_error_; }
  uint32_t id() const { return id_; }
  int edge() const { return slot_; }
  double seconds() const { return seconds_; }

 private:
  bool download_chunk(const Segment& seg);
  // Moves this worker to another edge; false when no edge is left.
  bool switch_edge();
  void note_error(const std::string& msg);

  uint32_t id_;
  WorkerContext ctx_;
  WorkQueue& queue_;
  FileSink& sink_;
  Progress& progress_;
  DownloadState& state_;
  std::atomic<bool>& cancel_;
  EndpointPool* pool_ = nullptr;
  int slot_ = -1;  // leased edge, -1 when not using a pool

  std::unique_ptr<HttpClient> client_;
  std::vector<char> buffer_;
  std::atomic<uint64_t> bytes_{0};
  std::atomic<uint32_t> errors_{0};
  std::string last_error_;
  double seconds_ = 0.0;
};

}  // namespace mtdl
