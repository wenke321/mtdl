#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "mtdl/net/endpoint.hpp"

namespace mtdl {

// Enumerates every address behind a hostname.
//
// Mirror DNS answers each query with a rotating subset of its records (here:
// 3-4 of ~12 addresses per lookup), so one getaddrinfo() reveals only a slice
// of the edge set. Resolving repeatedly and merging the results discovers the
// whole set - and that set is the unit of parallelism that matters when the
// server rate limits per edge rather than per client.
class Resolver {
 public:
  struct Options {
    uint16_t port = 443;
    int lookups = 24;          // repeated resolutions; each is one DNS query
    int parallel = 8;          // concurrent lookups (DNS is independent per query)
    size_t max_endpoints = 32; // hard stop (also bounds the downloader)
    bool ipv6 = true;          // keep AAAA records when the host has them
    bool literal_only = false; // never touch DNS, host must be an IP literal
  };

  static std::vector<Endpoint> resolve(const std::string& host, const Options& opts);

  // Accepts "1.2.3.4", "1.2.3.4:8080", "::1", "[::1]:443", or a bare hostname
  // (hostname is only accepted when allow_name is true, and then DNS is used).
  static bool parse(const std::string& spec, uint16_t default_port, Endpoint* out);
};

}  // namespace mtdl
