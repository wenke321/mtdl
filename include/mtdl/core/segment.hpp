#pragma once
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>

namespace mtdl {

class DownloadState;

// A contiguous piece of the file handed to one connection.
struct Segment {
  uint64_t offset = 0;
  uint64_t length = 0;
};

// Dynamic work source. Hands out runs of not-yet-downloaded blocks, with the
// chunk size shrinking as the file nears completion: this bounds the "tail"
// where only a couple of (slow, rate-limited) connections are still busy.
class WorkQueue {
 public:
  WorkQueue(uint64_t total, uint64_t block_size, size_t max_chunk_blocks,
            size_t parallelism, const DownloadState* state);

  bool next(Segment* out);

  // Hands an unfinished range back to the queue. Used when a worker gives up
  // on a connection so the bytes it never received are picked up by somebody
  // else instead of stalling the download.
  void requeue(const Segment& seg);

  uint64_t remaining_bytes() const;

 private:
  uint64_t total_ = 0;
  uint64_t block_ = 1;
  size_t max_chunk_ = 1;
  size_t parallelism_ = 1;
  const DownloadState* state_ = nullptr;
  uint64_t chunk_budget_locked() const;

  mutable std::mutex mu_;
  uint64_t cursor_ = 0;  // next block index to consider
  std::deque<Segment> retry_;  // requeued ranges, served before fresh blocks
};

}  // namespace mtdl
