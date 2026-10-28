#include "mtdl/core/config.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "mtdl/util/error.hpp"
#include "mtdl/util/strings.hpp"

namespace mtdl {
namespace {

[[noreturn]] void usage_error(const std::string& msg) {
  fail(ErrKind::Config, msg);
}

uint32_t parse_u32(const std::string& flag, const std::string& value) {
  char* end = nullptr;
  long v = std::strtol(value.c_str(), &end, 10);
  if (end == value.c_str() || v < 0 || v > 1000000)
    usage_error(flag + ": bad number '" + value + "'");
  return static_cast<uint32_t>(v);
}

std::vector<std::string> split_commas(const std::string& text) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= text.size()) {
    const size_t comma = text.find(',', start);
    const std::string item =
        str::trim(text.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
    if (!item.empty()) out.push_back(item);
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return out;
}

}  // namespace

void Config::print_usage(const char* argv0) {
  std::printf(
      "mtdl %s - multi-threaded HTTP(S) downloader (C++17, Linux)\n"
      "\n"
      "Usage:\n"
      "  %s <url> [options]\n"
      "\n"
      "Parallelism:\n"
      "  -c, --connections <n>    hard ceiling on connections (default 96)\n"
      "      --min-connections <n> adaptive ramp start point (default 8)\n"
      "      --conns-per-edge <n>  soft limit per mirror edge address (default 6)\n"
      "      --ramp-step <n>       connections added per probe (default: half the edges)\n"
      "      --ramp-interval <ms>  adaptive control period (default 1000)\n"
      "      --adaptive           add connections while throughput keeps rising (default)\n"
      "      --no-adaptive        launch every connection at once\n"
      "\n"
      "Mirror fan-out:\n"
      "      --fanout             use every resolved edge address (default)\n"
      "      --no-fanout          use a single address per host\n"
      "      --dns-lookups <n>     resolutions used to enumerate edges (default 24)\n"
      "      --endpoints <list>    explicit edges, e.g. 1.2.3.4,5.6.7.8:8080,[::1]\n"
      "      --connect-to <ip>     same, for a single edge\n"
      "\n"
      "Requests:\n"
      "  -o, --output <path>      output file (default: URL basename)\n"
      "  -s, --chunk-size <n>     max chunk per request, e.g. 2MiB (default: auto)\n"
      "      --retries <n>        retries per chunk (default 6)\n"
      "      --read-timeout <ms>  per-read timeout (default 5000)\n"
      "      --connect-timeout <ms> connect timeout (default 15000)\n"
      "      --rcvbuf <bytes>     SO_RCVBUF override (default: kernel autotuning)\n"
      "\n"
      "Transport:\n"
      "      --plain-http         force http:// even if the URL says https\n"
      "      --no-resume          ignore and delete previous partial state\n"
      "      --insecure           skip TLS certificate verification\n"
      "  -q, --quiet              only warnings and errors\n"
      "  -v, --verbose            debug logging\n"
      "  -h, --help               this text\n",
      "2.0", argv0);
}

Config Config::parse_args(int argc, char** argv) {
  Config cfg;
  bool have_url = false;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto need_value = [&](const char* flag) -> std::string {
      if (i + 1 >= argc) usage_error(std::string(flag) + " requires a value");
      return argv[++i];
    };

    if (a == "-h" || a == "--help") {
      print_usage(argv[0]);
      std::exit(0);
    } else if (a == "-o" || a == "--output") {
      cfg.output = need_value(a.c_str());
    } else if (a == "-c" || a == "--connections") {
      cfg.connections = parse_u32(a, need_value(a.c_str()));
    } else if (a == "--min-connections") {
      cfg.min_connections = parse_u32(a, need_value(a.c_str()));
    } else if (a == "--conns-per-edge") {
      cfg.conns_per_edge = parse_u32(a, need_value(a.c_str()));
    } else if (a == "-s" || a == "--chunk-size" || a == "--segment-size") {
      cfg.chunk_size = str::parse_size(need_value(a.c_str()));
    } else if (a == "--ramp") {
      cfg.ramp_ms = parse_u32(a, need_value(a.c_str()));
    } else if (a == "--ramp-step") {
      cfg.ramp_step = parse_u32(a, need_value(a.c_str()));
    } else if (a == "--ramp-interval") {
      cfg.ramp_interval_ms = std::max<uint32_t>(100, parse_u32(a, need_value(a.c_str())));
    } else if (a == "--plateau") {
      cfg.ramp_patience = std::max<uint32_t>(1, parse_u32(a, need_value(a.c_str())));
    } else if (a == "--retries") {
      cfg.max_retries = parse_u32(a, need_value(a.c_str()));
    } else if (a == "--read-timeout") {
      cfg.read_timeout_ms = static_cast<int>(parse_u32(a, need_value(a.c_str())));
    } else if (a == "--connect-timeout") {
      cfg.connect_timeout_ms = static_cast<int>(parse_u32(a, need_value(a.c_str())));
    } else if (a == "--rcvbuf") {
      cfg.rcvbuf = static_cast<int>(str::parse_size(need_value(a.c_str())));
    } else if (a == "--adaptive") {
      cfg.adaptive = true;
    } else if (a == "--no-adaptive") {
      cfg.adaptive = false;
    } else if (a == "--fanout") {
      cfg.fanout = true;
    } else if (a == "--no-fanout") {
      cfg.fanout = false;
    } else if (a == "--dns-lookups") {
      cfg.dns_lookups = static_cast<int>(parse_u32(a, need_value(a.c_str())));
    } else if (a == "--endpoints") {
      cfg.edges = split_commas(need_value(a.c_str()));
      if (cfg.edges.empty()) usage_error("--endpoints requires at least one address");
    } else if (a == "--connect-to") {
      cfg.connect_ip = need_value(a.c_str());
    } else if (a == "--plain-http") {
      cfg.force_plain_http = true;
    } else if (a == "--no-resume") {
      cfg.resume = false;
    } else if (a == "--insecure") {
      cfg.insecure = true;
    } else if (a == "-q" || a == "--quiet") {
      cfg.quiet = true;
      cfg.log_level = log::Level::Warn;
    } else if (a == "-v" || a == "--verbose") {
      cfg.log_level = log::Level::Debug;
    } else if (!a.empty() && a[0] == '-') {
      usage_error("unknown option: " + a);
    } else {
      if (have_url) usage_error("more than one URL given");
      cfg.url = a;
      have_url = true;
    }
  }

  if (!have_url) usage_error("no URL given (try --help)");
  if (cfg.connections == 0) usage_error("--connections must be >= 1");
  if (cfg.min_connections > cfg.connections) cfg.min_connections = cfg.connections;
  if (cfg.conns_per_edge == 0) cfg.conns_per_edge = 1;
  if (cfg.dns_lookups <= 0) cfg.dns_lookups = 1;
  return cfg;
}

}  // namespace mtdl
