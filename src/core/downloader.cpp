#include "mtdl/core/downloader.hpp"

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "mtdl/net/resolver.hpp"
#include "mtdl/util/error.hpp"
#include "mtdl/util/log.hpp"
#include "mtdl/util/strings.hpp"

namespace mtdl {
namespace {

constexpr const char* kUserAgent =
    "Mozilla/5.0 (X11; Linux x86_64; rv:128.0) Gecko/20100101 Firefox/128.0";
constexpr uint64_t kMinBlock = 64u << 10;    // resume granularity
constexpr uint64_t kMaxBitmap = 16384;       // keep the state file small
constexpr uint64_t kMinChunk = 128u << 10;   // smallest chunk per request
constexpr uint64_t kMaxChunk = 4u << 20;     // largest chunk per request

std::string basename_of(const std::string& target) {
  auto slash = target.find_last_of('/');
  std::string name = (slash == std::string::npos) ? target : target.substr(slash + 1);
  auto query = name.find('?');
  if (query != std::string::npos) name = name.substr(0, query);
  return name.empty() ? std::string("download.bin") : name;
}

uint64_t round_up(uint64_t v, uint64_t m) { return ((v + m - 1) / m) * m; }

}  // namespace

std::string Downloader::resolve_output_path() const {
  if (!cfg_.output.empty()) return cfg_.output;
  return basename_of(Url::parse(cfg_.url).target);
}

std::vector<Endpoint> Downloader::resolve_edges(const Url& url) const {
  const uint16_t port = url.port_num();

  // 1. explicit edge list (also used by the self test to exercise fan-out)
  if (!cfg_.edges.empty()) {
    std::vector<Endpoint> out;
    for (const auto& spec : cfg_.edges) {
      Endpoint ep;
      if (!Resolver::parse(spec, port, &ep))
        fail(ErrKind::Config, "--endpoints: '" + spec + "' is not an IP address");
      out.push_back(std::move(ep));
    }
    return out;
  }

  // 2. single pinned peer
  if (!cfg_.connect_ip.empty()) {
    Endpoint ep;
    if (!Resolver::parse(cfg_.connect_ip, port, &ep))
      fail(ErrKind::Config, "--connect-to: '" + cfg_.connect_ip + "' is not an IP address");
    return {ep};
  }

  // 3. the mirror's whole edge set
  Resolver::Options ro;
  ro.port = port;
  ro.lookups = cfg_.fanout ? cfg_.dns_lookups : 1;
  auto edges = Resolver::resolve(url.host, ro);
  if (!cfg_.fanout && edges.size() > 1) edges.resize(1);
  return edges;
}

DownloadReport Downloader::run() {
  DownloadReport report;
  const auto t_start = std::chrono::steady_clock::now();

  Url url = Url::parse(cfg_.url);
  if (cfg_.force_plain_http) url.scheme = "http";

  ClientOptions client_opts;
  client_opts.tls.verify = !cfg_.insecure;
  client_opts.connect_timeout_ms = cfg_.connect_timeout_ms;
  client_opts.read_timeout_ms = cfg_.read_timeout_ms;
  client_opts.rcvbuf = cfg_.rcvbuf;
  client_opts.connect_ip = cfg_.connect_ip;
  client_opts.user_agent = kUserAgent;

  // ---- 1. probe ----------------------------------------------------------
  Prober prober(url, client_opts);
  const ProbeResult probe = prober.probe();
  if (cancel_.load()) {
    report.cancelled = true;
    return report;
  }

  // ---- 2. plan -----------------------------------------------------------
  uint32_t conns = cfg_.connections;
  if (!probe.range_supported) {
    conns = 1;
    LOG_WARN("no byte-range support: falling back to a single connection");
  }

  // Resume granularity: small blocks (fine resume, fine-grained tails), grown
  // only if the bitmap would get large.
  uint64_t block = kMinBlock;
  while (probe.size / block > kMaxBitmap) block <<= 1;

  uint64_t chunk = cfg_.chunk_size;
  if (chunk == 0) {
    const uint64_t fair = probe.size / (static_cast<uint64_t>(conns) * 4);
    chunk = std::min(std::max(fair, kMinChunk), kMaxChunk);
  }
  chunk = round_up(std::max(chunk, block), block);

  // ---- 3. resume state ---------------------------------------------------
  const std::string out_path = resolve_output_path();
  const std::string part_path = out_path + ".part";
  const std::string state_path = out_path + ".mtdl-state";

  DownloadState state;
  uint64_t already = 0;
  const bool part_exists = ::access(part_path.c_str(), F_OK) == 0;
  if (cfg_.resume && part_exists) {
    state = DownloadState::load(state_path);
    if (state.matches(url.str(), probe.size) && state.block_size() == block) {
      already = state.bytes_done();
      if (already > 0)
        LOG_INFO("resuming: " << str::format_bytes(already) << " already on disk");
    } else {
      state = DownloadState();
    }
  } else {
    // no partial file (or resume disabled): start from scratch
    ::unlink(state_path.c_str());
    if (!cfg_.resume) ::unlink(part_path.c_str());
  }
  if (already == 0) state.init(url.str(), probe.size, block);

  // ---- 4. sink, queue, edges, progress -----------------------------------
  FileSink sink;
  sink.open(part_path, probe.size, /*preallocate=*/already == 0);

  WorkQueue queue(probe.size, block, static_cast<size_t>(chunk / block), conns, &state);

  std::vector<Endpoint> edges = resolve_edges(url);
  if (!probe.range_supported) edges.resize(1);
  EndpointPool pool(edges, cfg_.conns_per_edge);
  LOG_INFO("plan: block=" << str::format_bytes(block) << " chunk<=" << str::format_bytes(chunk)
                          << " connections<=" << conns << " edges=" << edges.size()
                          << (edges.size() > 1 ? " (fan-out)" : ""));
  for (size_t i = 0; i < edges.size() && i < 16; ++i)
    LOG_DEBUG("edge[" << i << "] " << edges[i].str());

  Progress progress(probe.size, 0);
  progress.seed(already);

  WorkerContext ctx;
  ctx.url = url;
  ctx.client = client_opts;
  ctx.max_retries = cfg_.max_retries;
  ctx.total_size = probe.size;

  std::vector<std::unique_ptr<Worker>> workers;
  std::vector<std::thread> threads;
  workers.reserve(conns);
  threads.reserve(conns);

  auto spawn_one = [&]() {
    const uint32_t id = static_cast<uint32_t>(workers.size());
    auto worker = std::make_unique<Worker>(id, ctx, queue, sink, progress, state, cancel_, &pool);
    Worker* raw = worker.get();
    workers.push_back(std::move(worker));
    progress.inc_active();
    threads.emplace_back([raw]() {
      try {
        raw->run();
      } catch (const std::exception& e) {
        LOG_ERROR("worker crashed: " << e.what());
      }
    });
  };

  progress.start();

  const uint32_t start_conns =
      std::min(cfg_.min_connections > 0 ? cfg_.min_connections : conns, conns);
  LOG_INFO("starting " << start_conns << " connection(s)"
                       << (cfg_.adaptive ? " with adaptive ramp" : ""));
  for (uint32_t i = 0; i < start_conns; ++i) {
    spawn_one();
    if (cfg_.ramp_ms > 0 && i + 1 < start_conns)
      std::this_thread::sleep_for(std::chrono::milliseconds(cfg_.ramp_ms));
  }

  // ---- 5. adaptive ramp ---------------------------------------------------
  // Add connections while measured throughput still rises. This is what finds
  // the knee: the mirror hands out a fixed byte rate per connection (and, in
  // some regimes, per edge), and beyond the aggregate ceiling more sockets only
  // add handshakes. The allowance drifts over time, so a client that merely
  // starts N connections either wastes sockets or leaves bandwidth on the
  // table. Measurement is an EMA over control periods, so a single noisy
  // sample cannot end the ramp.
  if (cfg_.adaptive && start_conns < conns) {
    double ema = 0.0;
    double best = 0.0;
    bool have_ema = false;
    int stagnant = 0;
    uint64_t prev_bytes = progress.bytes();
    auto prev_t = std::chrono::steady_clock::now();
    pool.sample_rates();

    while (progress.active() < conns && !cancel_.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(cfg_.ramp_interval_ms));
      if (cancel_.load()) break;
      if (queue.remaining_bytes() == 0) break;  // nothing left to fetch

      const auto now = std::chrono::steady_clock::now();
      const uint64_t cur = progress.bytes();
      const double dt = std::chrono::duration<double>(now - prev_t).count();
      const double inst =
          dt > 0.0 ? (static_cast<double>(cur - prev_bytes) / (1024.0 * 1024.0)) / dt : 0.0;
      prev_bytes = cur;
      prev_t = now;
      pool.sample_rates();
      ema = have_ema ? 0.6 * ema + 0.4 * inst : inst;
      have_ema = true;

      const uint32_t live = progress.active();
      if (ema > best * 1.03 + 0.02) {
        best = ema;
        stagnant = 0;
        const uint32_t room = conns - std::min(conns, live);
        const uint32_t auto_step =
            static_cast<uint32_t>(std::max<size_t>(2, pool.healthy() / 2));
        const uint32_t step = std::min({cfg_.ramp_step ? cfg_.ramp_step : auto_step, room,
                                        std::max<uint32_t>(1, conns - live)});
        if (step == 0) break;
        LOG_INFO("adaptive: " << str::format_speed(ema) << " over " << live << " conn(s) / "
                              << pool.healthy() << " edge(s) -> +" << step);
        for (uint32_t i = 0; i < step; ++i) spawn_one();
      } else if (++stagnant >= static_cast<int>(cfg_.ramp_patience)) {
        LOG_INFO("adaptive: plateau at " << str::format_speed(best) << " over " << live
                                         << " conn(s)");
        break;
      }
    }
  } else if (!cfg_.adaptive) {
    for (uint32_t live = start_conns; live < conns; ++live) {
      spawn_one();
      if (cfg_.ramp_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(cfg_.ramp_ms));
    }
  }

  // ---- 6. wait -----------------------------------------------------------
  for (auto& t : threads) t.join();
  progress.stop();

  sink.sync();
  sink.close();

  // ---- 7. verify & finalise ---------------------------------------------
  const bool complete = state.done_blocks() == state.blocks();
  if (complete) {
    if (::rename(part_path.c_str(), out_path.c_str()) != 0)
      fail(ErrKind::Io, "cannot rename '" + part_path + "' to '" + out_path + "'");
    ::unlink(state_path.c_str());
    LOG_INFO("saved to " << out_path);
  } else {
    state.save(state_path);
    LOG_WARN("incomplete: " << state.done_blocks() << "/" << state.blocks()
                            << " blocks on disk; re-run the same command to resume");
  }

  if (log::level() == log::Level::Debug) {
    pool.sample_rates();
    for (size_t i = 0; i < pool.size(); ++i) {
      const auto v = pool.view(i);
      LOG_DEBUG("edge " << v.ep.str() << ": " << str::format_bytes(v.bytes) << ", "
                        << str::format_speed(v.mibps) << (v.dead ? " (retired)" : ""));
    }
    for (const auto& w : workers)
      if (w->bytes() > 0)
        LOG_DEBUG("conn " << w->id() << ": " << str::format_bytes(w->bytes()) << " at "
                          << str::format_speed(w->bytes() / 1048576.0 /
                                               std::max(0.001, w->seconds()))
                          << ", errors=" << w->errors());
  }

  report.bytes = progress.bytes();
  report.seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
  report.completed = complete;
  report.cancelled = cancel_.load();
  report.connections = static_cast<uint32_t>(workers.size());
  report.edges = pool.size();
  report.average_speed =
      report.seconds > 0
          ? (static_cast<double>(report.bytes) / (1024.0 * 1024.0)) / report.seconds
          : 0.0;
  return report;
}

}  // namespace mtdl
