#include "mtdl/core/progress.hpp"

#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "mtdl/util/strings.hpp"

namespace mtdl {
namespace {
constexpr double kWindowSeconds = 2.0;  // smoothing window for the live rate
}

Progress::Progress(uint64_t total, uint32_t connections) : total_(total) {
  active_.store(connections, std::memory_order_relaxed);
  t0_ = std::chrono::steady_clock::now();
}

Progress::~Progress() { stop(); }

void Progress::start() {
  stop_.store(false);
  thread_ = std::thread([this] { loop(); });
}

void Progress::stop() {
  if (thread_.joinable()) {
    stop_.store(true);
    thread_.join();
  }
}

double Progress::elapsed() const {
  auto now = std::chrono::steady_clock::now();
  return std::chrono::duration<double>(now - t0_).count();
}

double Progress::average_speed() const {
  const double secs = elapsed();
  if (secs <= 0.0) return 0.0;
  return (static_cast<double>(bytes()) / (1024.0 * 1024.0)) / secs;
}

double Progress::instant_speed() const {
  std::lock_guard<std::mutex> lock(mu_);
  if (samples_.size() < 2) return average_speed();
  const auto& first = samples_.front();
  const auto& last = samples_.back();
  const double dt = last.first - first.first;
  if (dt <= 0.0) return 0.0;
  const double db = static_cast<double>(last.second - first.second);
  return (db / (1024.0 * 1024.0)) / dt;
}

void Progress::loop() {
  const bool tty = ::isatty(STDERR_FILENO) != 0;
  uint64_t last_bytes = bytes();
  double last_t = elapsed();
  const double interval = tty ? 0.25 : 2.0;

  while (!stop_.load(std::memory_order_relaxed)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(interval * 1000)));
    const double t = elapsed();
    const uint64_t cur = bytes();
    const uint64_t shown = seed_.load(std::memory_order_relaxed) + cur;

    {
      std::lock_guard<std::mutex> lock(mu_);
      samples_.emplace_back(t, cur);
      while (!samples_.empty() && t - samples_.front().first > kWindowSeconds)
        samples_.pop_front();
    }
    (void)last_bytes;
    (void)last_t;

    const double rate = instant_speed();
    const double pct = total_ > 0 ? 100.0 * static_cast<double>(shown) / static_cast<double>(total_)
                                  : 0.0;
    const double remaining = total_ > shown ? static_cast<double>(total_ - shown) : 0.0;
    const double eta = rate > 0.01 ? remaining / (1024.0 * 1024.0) / rate : -1.0;

    std::string line = str::format_bytes(shown) + " / " + str::format_bytes(total_);
    std::string text;
    char buf[256];
    std::snprintf(buf, sizeof buf, "%5.1f%%  %s  %s  eta %s  conns %u", pct, line.c_str(),
                  str::format_speed(rate).c_str(), str::format_duration(eta).c_str(),
                  active_.load(std::memory_order_relaxed));
    text = buf;

    if (tty) {
      std::fprintf(stderr, "\r\033[K%s", text.c_str());
      std::fflush(stderr);
    } else {
      std::fprintf(stderr, "%s\n", text.c_str());
      std::fflush(stderr);
    }
  }
  if (tty) std::fprintf(stderr, "\r\033[K"), std::fflush(stderr);
}

}  // namespace mtdl
