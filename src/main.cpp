#include <csignal>
#include <cstdio>
#include <exception>

#include <atomic>

#include "mtdl/core/config.hpp"
#include "mtdl/core/downloader.hpp"
#include "mtdl/util/error.hpp"
#include "mtdl/util/log.hpp"
#include "mtdl/util/strings.hpp"

namespace {
std::atomic<bool> g_cancel{false};

void on_signal(int sig) {
  (void)sig;
  g_cancel.store(true, std::memory_order_relaxed);
}

void install_signal_handlers() {
  struct sigaction sa {};
  sa.sa_handler = on_signal;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
  std::signal(SIGPIPE, SIG_IGN);
}
}  // namespace

int main(int argc, char** argv) {
  try {
    mtdl::Config cfg = mtdl::Config::parse_args(argc, argv);
    mtdl::log::set_level(cfg.log_level);
    install_signal_handlers();

    mtdl::Downloader downloader(cfg, g_cancel);
    const mtdl::DownloadReport rep = downloader.run();

    std::printf("\n--- summary ---\n");
    std::printf("url        : %s\n", cfg.url.c_str());
    std::printf("downloaded : %s\n", mtdl::str::format_bytes(rep.bytes).c_str());
    std::printf("time       : %.2f s\n", rep.seconds);
    std::printf("avg speed  : %s\n", mtdl::str::format_speed(rep.average_speed).c_str());
    std::printf("connections: %u\n", rep.connections);
    std::printf("edges      : %zu\n", rep.edges);
    std::printf("status     : %s\n", rep.completed   ? "complete"
                                       : rep.cancelled ? "cancelled (state saved)"
                                                       : "incomplete (state saved)");
    if (rep.completed) return 0;
    return rep.cancelled ? 130 : 1;
  } catch (const mtdl::Error& e) {
    std::fprintf(stderr, "mtdl: %s\n", e.what());
    return 2;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "mtdl: unexpected error: %s\n", e.what());
    return 2;
  }
}
