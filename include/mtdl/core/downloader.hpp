#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "mtdl/core/config.hpp"
#include "mtdl/core/endpoint_pool.hpp"
#include "mtdl/core/file_sink.hpp"
#include "mtdl/core/prober.hpp"
#include "mtdl/core/progress.hpp"
#include "mtdl/core/segment.hpp"
#include "mtdl/core/state.hpp"
#include "mtdl/core/worker.hpp"

namespace mtdl {

struct DownloadReport {
  uint64_t bytes = 0;
  double seconds = 0.0;
  bool completed = false;
  bool cancelled = false;
  uint32_t connections = 0;
  size_t edges = 0;
  double average_speed = 0.0;  // MiB/s
};

// Orchestrates probe -> plan -> edges -> workers -> resume/state -> finalise.
class Downloader {
 public:
  Downloader(Config cfg, std::atomic<bool>& cancel)
      : cfg_(std::move(cfg)), cancel_(cancel) {}

  DownloadReport run();

 private:
  std::string resolve_output_path() const;
  std::vector<Endpoint> resolve_edges(const Url& url) const;

  Config cfg_;
  std::atomic<bool>& cancel_;
};

}  // namespace mtdl
