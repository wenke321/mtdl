#pragma once
#include <cstdint>
#include <string>

namespace mtdl {

// A concrete TCP peer: one resolved edge address plus the port to use.
//
// Mirror hostnames publish a *set* of edge addresses (rotating A/AAAA records).
// A single edge is usually rate limited independently of the others, so the
// downloader spreads its connections over the whole set instead of letting the
// kernel pick one address per DNS lookup. `ipv6` avoids dragging <sys/socket.h>
// (and AF_INET6) through every header that only wants to name a peer.
struct Endpoint {
  std::string ip;
  uint16_t port = 0;
  bool ipv6 = false;

  bool valid() const { return !ip.empty() && port != 0; }

  std::string str() const {
    if (ipv6) return "[" + ip + "]:" + std::to_string(port);
    return ip + ":" + std::to_string(port);
  }
};

inline bool same_peer(const Endpoint& a, const Endpoint& b) {
  return a.port == b.port && a.ip == b.ip && a.ipv6 == b.ipv6;
}

}  // namespace mtdl
