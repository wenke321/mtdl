#include "mtdl/core/segment.hpp"

#include <algorithm>

#include "mtdl/core/state.hpp"

namespace mtdl {

WorkQueue::WorkQueue(uint64_t total, uint64_t block_size, size_t max_chunk_blocks,
                     size_t parallelism, const DownloadState* state)
    : total_(total),
      block_(block_size ? block_size : 1),
      max_chunk_(std::max<size_t>(1, max_chunk_blocks)),
      parallelism_(std::max<size_t>(1, parallelism)),
      state_(state) {}

uint64_t WorkQueue::chunk_budget_locked() const {
  const uint64_t blocks_total = (total_ + block_ - 1) / block_;
  const uint64_t left = cursor_ >= blocks_total ? 0 : blocks_total - cursor_;
  // Shrink chunks as the file finishes so idle connections keep getting work
  // instead of watching the last few stragglers crawl to the end.
  const uint64_t fair = left / (4 * static_cast<uint64_t>(parallelism_));
  const uint64_t want = std::min<uint64_t>(max_chunk_, std::max<uint64_t>(1, fair));
  return want * block_;
}

void WorkQueue::requeue(const Segment& seg) {
  if (seg.length == 0) return;
  std::lock_guard<std::mutex> lock(mu_);
  uint64_t offset = seg.offset;
  uint64_t length = seg.length;
  if (offset >= total_) return;
  if (offset + length > total_) length = total_ - offset;
  retry_.push_back(Segment{offset, length});
}

bool WorkQueue::next(Segment* out) {
  std::lock_guard<std::mutex> lock(mu_);

  // Requeued ranges first: somebody already tried them and ran out of luck.
  while (!retry_.empty()) {
    Segment seg = retry_.front();
    retry_.pop_front();
    const uint64_t budget = chunk_budget_locked();
    if (seg.length > budget) {
      const uint64_t rest = seg.length - budget;
      retry_.push_front(Segment{seg.offset + budget, rest});
      seg.length = budget;
    }
    *out = seg;
    return true;
  }

  const uint64_t blocks_total = (total_ + block_ - 1) / block_;
  for (;;) {
    // skip blocks that are already on disk
    while (cursor_ < blocks_total && state_ &&
           state_->is_block_done(static_cast<size_t>(cursor_)))
      ++cursor_;
    if (cursor_ >= blocks_total) return false;

    const uint64_t want_blocks = std::max<uint64_t>(1, chunk_budget_locked() / block_);
    const uint64_t start = cursor_;
    uint64_t take = 0;
    while (cursor_ < blocks_total && take < want_blocks) {
      if (state_ && state_->is_block_done(static_cast<size_t>(cursor_))) break;
      ++cursor_;
      ++take;
    }
    if (take == 0) continue;  // nothing claimable here; rescan

    out->offset = start * block_;
    const uint64_t bytes = take * block_;
    out->length = std::min(bytes, total_ - out->offset);
    return true;
  }
}

uint64_t WorkQueue::remaining_bytes() const {
  std::lock_guard<std::mutex> lock(mu_);
  uint64_t retry = 0;
  for (const auto& s : retry_) retry += s.length;
  const uint64_t blocks_total = (total_ + block_ - 1) / block_;
  if (cursor_ >= blocks_total) return retry;
  const uint64_t left = (blocks_total - cursor_) * block_;
  return retry + (left > total_ ? total_ : left);
}

}  // namespace mtdl
