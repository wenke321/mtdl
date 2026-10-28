#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>

namespace mtdl {

// Samples the shared byte counter and prints a one-line live status.
class Progress {
 public:
  Progress(uint64_t total, uint32_t connections);
  ~Progress();

  void start();
  void stop();

  void add(uint64_t bytes) { bytes_.fetch_add(bytes, std::memory_order_relaxed); }
  // Bytes already on disk from a previous run (counted in the percentage only).
  void seed(uint64_t bytes) { seed_.store(bytes); }
  uint64_t bytes() const { return bytes_.load(std::memory_order_relaxed); }

  double average_speed() const;   // MiB/s since start
  double instant_speed() const;   // MiB/s over the last sampling window
  double elapsed() const;
  uint32_t active() const { return active_.load(std::memory_order_relaxed); }
  void set_active_connections(uint32_t n) { active_.store(n); }
  void inc_active() { active_.fetch_add(1); }
  void dec_active() {
    uint32_t cur = active_.load();
    while (cur > 0 && !active_.compare_exchange_weak(cur, cur - 1)) {
    }
  }

 private:
  void loop();

  uint64_t total_;
  std::atomic<uint64_t> bytes_{0};
  std::atomic<uint64_t> seed_{0};
  std::atomic<uint32_t> active_{0};
  std::atomic<bool> stop_{false};
  std::chrono::steady_clock::time_point t0_;
  std::thread thread_;
  mutable std::mutex mu_;
  std::deque<std::pair<double, uint64_t>> samples_;  // (seconds, bytes)
};

}  // namespace mtdl
