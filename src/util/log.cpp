#include "mtdl/util/log.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>

namespace mtdl::log {
namespace {

std::atomic<Level> g_level{Level::Info};
std::mutex g_mu;

const char* tag(Level lv) {
  switch (lv) {
    case Level::Debug: return "DBG";
    case Level::Info: return "INF";
    case Level::Warn: return "WRN";
    case Level::Error: return "ERR";
  }
  return "???";
}

}  // namespace

void set_level(Level lv) noexcept { g_level.store(lv, std::memory_order_relaxed); }

Level level() noexcept { return g_level.load(std::memory_order_relaxed); }

void write(Level lv, const std::string& msg) {
  using namespace std::chrono;
  auto now = system_clock::now();
  auto secs = system_clock::to_time_t(now);
  auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
  std::tm tmv{};
  localtime_r(&secs, &tmv);

  std::lock_guard<std::mutex> lock(g_mu);
  std::fprintf(stderr, "[%02d:%02d:%02d.%03d] %s %s\n", tmv.tm_hour, tmv.tm_min,
               tmv.tm_sec, static_cast<int>(ms), tag(lv), msg.c_str());
  std::fflush(stderr);
}

}  // namespace mtdl::log
