#include "mtdl/core/worker.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

#include "mtdl/net/http/response.hpp"
#include "mtdl/util/error.hpp"
#include "mtdl/util/log.hpp"
#include "mtdl/util/strings.hpp"

namespace mtdl {
namespace {
// 256 KiB is enough to keep the socket busy even at gigabit rates; keeping it
// small lets us run 100+ workers without wasting memory.
constexpr size_t kReadBufferSize = 256 << 10;
}  // namespace

Worker::Worker(uint32_t id, const WorkerContext& ctx, WorkQueue& queue, FileSink& sink,
               Progress& progress, DownloadState& state, std::atomic<bool>& cancel,
               EndpointPool* pool)
    : id_(id),
      ctx_(ctx),
      queue_(queue),
      sink_(sink),
      progress_(progress),
      state_(state),
      cancel_(cancel),
      pool_(pool) {
  buffer_.resize(kReadBufferSize);
}

void Worker::note_error(const std::string& msg) {
  last_error_ = msg;
  errors_.fetch_add(1, std::memory_order_relaxed);
}

bool Worker::switch_edge() {
  if (pool_ == nullptr || slot_ < 0) return false;
  pool_->note_failure(slot_);
  pool_->release(slot_);
  slot_ = pool_->acquire();
  if (slot_ < 0) return false;
  if (client_) client_->disconnect();
  return true;
}

void Worker::run() {
  const auto started = std::chrono::steady_clock::now();

  if (pool_ != nullptr) {
    slot_ = pool_->acquire();
    if (slot_ < 0) {
      LOG_DEBUG("worker " << id_ << ": every edge is retired, nothing to do");
      progress_.dec_active();
      seconds_ =
          std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
      return;
    }
    LOG_DEBUG("worker " << id_ << " -> edge " << pool_->endpoint(static_cast<size_t>(slot_)).str());
  }

  Segment chunk;
  while (!cancel_.load(std::memory_order_relaxed) && queue_.next(&chunk)) {
    if (!download_chunk(chunk)) {
      if (!cancel_.load(std::memory_order_relaxed))
        LOG_ERROR("worker " << id_ << " gave up at offset " << chunk.offset << ": "
                            << last_error_);
      break;
    }
  }
  if (client_) client_->disconnect();
  if (pool_ != nullptr && slot_ >= 0) pool_->release(slot_);
  progress_.dec_active();
  seconds_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  LOG_DEBUG("worker " << id_ << " finished: " << str::format_bytes(bytes()) << " in "
                      << seconds_ << "s ("
                      << str::format_speed(bytes() / 1048576.0 / std::max(0.001, seconds_))
                      << "), " << errors() << " error(s)");
}

// Downloads [seg.offset, seg.offset+seg.length) marking each completed block in
// the resume bitmap. Retries continue from the last byte written, so a dropped
// connection never costs more than the data already received.
bool Worker::download_chunk(const Segment& seg) {
  const uint64_t block = state_.block_size();
  const uint64_t seg_end = seg.offset + seg.length;
  uint64_t written = 0;
  uint64_t marked_upto = seg.offset;  // absolute position up to which blocks are marked
  uint32_t attempt = 0;
  uint32_t failures_on_edge = 0;

  while (seg.offset + written < seg_end) {
    if (cancel_.load(std::memory_order_relaxed)) return false;
    try {
      if (!client_) client_ = std::make_unique<HttpClient>(ctx_.url, ctx_.client);
      if (pool_ != nullptr && slot_ >= 0 && !client_->has_pinned_endpoint())
        client_->set_endpoint(pool_->endpoint(static_cast<size_t>(slot_)));
      if (!client_->connected() || !client_->reusable()) client_->connect();

      const uint64_t from = seg.offset + written;
      const uint64_t to = seg_end - 1;
      client_->request_range(from, to);

      const int status = client_->head().status();
      if (status != 206 && !(status == 200 && from == 0)) {
        fail(ErrKind::Protocol, "unexpected HTTP " + std::to_string(status) + " for range " +
                                    std::to_string(from) + "-" + std::to_string(to));
      }

      for (;;) {
        if (cancel_.load(std::memory_order_relaxed)) return false;
        const ssize_t got = client_->read_some(buffer_.data(), buffer_.size());
        if (got <= 0) break;
        const uint64_t left = seg_end - (seg.offset + written);
        const size_t take =
            static_cast<size_t>(std::min<uint64_t>(left, static_cast<uint64_t>(got)));
        sink_.write_at(seg.offset + written, buffer_.data(), take);
        written += take;
        bytes_.fetch_add(take, std::memory_order_relaxed);
        progress_.add(take);
        if (pool_ != nullptr && slot_ >= 0) pool_->add_bytes(slot_, take);
        failures_on_edge = 0;

        // Publish completed blocks so an interrupted run loses as little as possible.
        // The final block of the file is shorter than `block`, hence the min().
        // Both bounds matter: stop at the write position (a block is only done
        // once its last byte is on disk) *and* at the end of the file, so the
        // loop always advances instead of re-marking the final block forever.
        const uint64_t abs_written = seg.offset + written;
        while (marked_upto < abs_written && marked_upto < ctx_.total_size) {
          const size_t idx = static_cast<size_t>(marked_upto / block);
          const uint64_t boundary =
              std::min<uint64_t>((static_cast<uint64_t>(idx) + 1) * block, ctx_.total_size);
          if (abs_written < boundary) break;  // block not fully on disk yet
          state_.mark_block_done(idx);
          marked_upto = boundary;  // boundary > marked_upto here, so this advances
        }
        if (seg.offset + written >= seg_end) break;
      }

      if (seg.offset + written < seg_end)
        fail(ErrKind::Protocol, "short read at " + std::to_string(seg.offset + written) + " of " +
                                    std::to_string(seg_end));
      return true;
    } catch (const Error& e) {
      note_error(e.what());
      ++attempt;
      if (client_) client_->disconnect();
      if (cancel_.load(std::memory_order_relaxed)) return false;

      // An edge that cannot even deliver the first byte of a chunk is not worth
      // keeping: move this worker to another edge (and retire a dead one).
      if (pool_ != nullptr && written == 0 && ++failures_on_edge >= 2) {
        failures_on_edge = 0;
        if (switch_edge()) {
          attempt = 0;
          continue;
        }
      }

      if (attempt > ctx_.max_retries) {
        // Give the unread tail back so another connection can finish it.
        const uint64_t from = seg.offset + written;
        if (from < seg_end) queue_.requeue(Segment{from, seg_end - from});
        return false;
      }

      const uint32_t backoff = std::min<uint32_t>(2000, 100u << std::min<uint32_t>(attempt, 4));
      LOG_DEBUG("worker " << id_ << " retry " << attempt << "/" << ctx_.max_retries << " at "
                          << seg.offset + written << ": " << e.what());
      for (uint32_t slept = 0; slept < backoff; slept += 50) {
        if (cancel_.load(std::memory_order_relaxed)) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
    }
  }
  return true;
}

}  // namespace mtdl
