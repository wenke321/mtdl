#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "mtdl/util/log.hpp"

namespace mtdl {

struct Config {
  std::string url;
  std::string output;  // empty -> derived from the URL basename

  // ---- parallelism -------------------------------------------------------
  uint32_t connections = 96;      // hard ceiling on worker threads / sockets
  uint32_t min_connections = 8;   // adaptive ramp starts here
  uint32_t conns_per_edge = 6;    // soft limit per edge address
  bool adaptive = true;           // grow the connection count while it pays off
  uint32_t ramp_ms = 25;          // delay between the initial launches
  uint32_t ramp_interval_ms = 1000;  // adaptive control period
  uint32_t ramp_step = 0;         // 0 -> auto (half the healthy edge count)
  uint32_t ramp_patience = 2;     // stagnant samples before declaring a plateau

  // ---- mirror edge fan-out ----------------------------------------------
  bool fanout = true;             // spread connections over every resolved A/AAAA
  int dns_lookups = 24;           // repeated resolutions used to enumerate edges
  std::vector<std::string> edges; // --endpoints: explicit edge list (no DNS)

  // ---- request shaping ---------------------------------------------------
  uint64_t chunk_size = 0;        // 0 -> auto (size / (connections * 4))
  uint32_t max_retries = 6;
  int read_timeout_ms = 5000;  // also bounds Ctrl-C latency
  int connect_timeout_ms = 15000;
  int rcvbuf = 0;

  // ---- transport / misc --------------------------------------------------
  bool insecure = false;
  bool resume = true;
  bool force_plain_http = false;  // ignore https, use port 80
  std::string connect_ip;         // pin the TCP peer (SNI/Host stay unchanged)
  bool quiet = false;
  log::Level log_level = log::Level::Info;

  static Config parse_args(int argc, char** argv);
  static void print_usage(const char* argv0);
};

}  // namespace mtdl
