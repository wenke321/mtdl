#include "mtdl/net/resolver.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_set>

#include "mtdl/util/error.hpp"
#include "mtdl/util/log.hpp"

namespace mtdl {
namespace {

// Splits "[v6]:port" / "v4:port" / plain address. Returns false if `text` is
// neither an IPv4 nor an IPv6 literal.
bool split_literal(const std::string& text, uint16_t default_port, Endpoint* out) {
  std::string host = text;
  std::string port_text;
  if (!text.empty() && text.front() == '[') {
    const auto close = text.find(']');
    if (close == std::string::npos) return false;
    host = text.substr(1, close - 1);
    if (close + 1 < text.size()) {
      if (text[close + 1] != ':') return false;
      port_text = text.substr(close + 2);
    }
  } else if (text.find(':') != std::string::npos &&
             text.find(':') == text.rfind(':')) {
    // Exactly one colon: "1.2.3.4:8080". A bare IPv6 literal has two or more.
    host = text.substr(0, text.find(':'));
    port_text = text.substr(text.find(':') + 1);
  }

  Endpoint ep;
  ep.port = default_port;
  if (!port_text.empty()) {
    char* end = nullptr;
    long p = std::strtol(port_text.c_str(), &end, 10);
    if (end == port_text.c_str() || *end != '\0' || p <= 0 || p > 65535) return false;
    ep.port = static_cast<uint16_t>(p);
  }

  in_addr v4{};
  if (::inet_pton(AF_INET, host.c_str(), &v4) == 1) {
    ep.ip = host;
    ep.ipv6 = false;
    *out = std::move(ep);
    return true;
  }
  in6_addr v6{};
  if (::inet_pton(AF_INET6, host.c_str(), &v6) == 1) {
    ep.ip = host;
    ep.ipv6 = true;
    *out = std::move(ep);
    return true;
  }
  return false;
}

bool addr_to_endpoint(const addrinfo* ai, uint16_t port, Endpoint* out) {
  char text[INET6_ADDRSTRLEN] = {0};
  if (ai->ai_family == AF_INET) {
    const auto* sa = reinterpret_cast<const sockaddr_in*>(ai->ai_addr);
    if (::inet_ntop(AF_INET, &sa->sin_addr, text, sizeof text) == nullptr) return false;
    out->ipv6 = false;
  } else if (ai->ai_family == AF_INET6) {
    const auto* sa = reinterpret_cast<const sockaddr_in6*>(ai->ai_addr);
    if (::inet_ntop(AF_INET6, &sa->sin6_addr, text, sizeof text) == nullptr) return false;
    out->ipv6 = true;
  } else {
    return false;
  }
  out->ip = text;
  out->port = port;
  return true;
}

}  // namespace

bool Resolver::parse(const std::string& spec, uint16_t default_port, Endpoint* out) {
  return split_literal(spec, default_port, out);
}

std::vector<Endpoint> Resolver::resolve(const std::string& host, const Options& opts) {
  std::vector<Endpoint> out;

  // IP literal (or --connect-to): no DNS, exactly one edge.
  Endpoint literal;
  if (split_literal(host, opts.port, &literal)) {
    out.push_back(std::move(literal));
    return out;
  }
  if (opts.literal_only)
    fail(ErrKind::Config, "'" + host + "' is not an IP address");

  addrinfo hints{};
  hints.ai_family = opts.ipv6 ? AF_UNSPEC : AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  // AI_ADDRCONFIG keeps unreachable address families out of the edge set: on a
  // host without IPv6 routing, AAAA records would otherwise cost a connect
  // timeout per edge before being retired.
  hints.ai_flags = AI_ADDRCONFIG;
  const std::string port_text = std::to_string(opts.port);

  std::unordered_set<std::string> seen;
  std::mutex mu;
  std::string last_error;

  // Each lookup is one independent DNS query, so run several at a time: the
  // whole edge set is then known in one round-trip instead of `lookups` of them.
  const int rounds = std::max(1, opts.lookups);
  const int workers = std::max(1, std::min<int>(opts.parallel, rounds));

  auto query_once = [&]() {
    addrinfo* res = nullptr;
    const int rc = ::getaddrinfo(host.c_str(), port_text.c_str(), &hints, &res);
    if (rc != 0) {
      std::lock_guard<std::mutex> lock(mu);
      last_error = ::gai_strerror(rc);
      return;
    }
    std::vector<Endpoint> found;
    for (const addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
      Endpoint ep;
      if (addr_to_endpoint(ai, opts.port, &ep)) found.push_back(std::move(ep));
    }
    ::freeaddrinfo(res);

    std::lock_guard<std::mutex> lock(mu);
    for (auto& ep : found) {
      if (out.size() >= opts.max_endpoints) break;
      const std::string key = (ep.ipv6 ? "6/" : "4/") + ep.ip;
      if (seen.insert(key).second) out.push_back(std::move(ep));
    }
  };

  auto worker = [&](int n) {
    for (int i = 0; i < n; ++i) {
      {
        std::lock_guard<std::mutex> lock(mu);
        if (out.size() >= opts.max_endpoints) return;
      }
      query_once();
    }
  };

  if (workers == 1) {
    worker(rounds);
  } else {
    std::vector<std::thread> pool;
    pool.reserve(static_cast<size_t>(workers));
    const int per = (rounds + workers - 1) / workers;
    for (int i = 0; i < workers; ++i) pool.emplace_back(worker, per);
    for (auto& t : pool) t.join();
  }

  if (out.empty()) {
    fail(ErrKind::Network,
         "resolve " + host + ": " + (last_error.empty() ? "no address records" : last_error));
  }
  return out;
}

}  // namespace mtdl
