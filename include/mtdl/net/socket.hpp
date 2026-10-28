#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <sys/types.h>

#include "mtdl/net/endpoint.hpp"

namespace mtdl {

// Owns a connected TCP socket. Move-only.
class TcpSocket {
 public:
  struct Options {
    int connect_timeout_ms = 15000;
    int read_timeout_ms = 30000;
    int rcvbuf = 0;  // 0 -> leave kernel autotuning alone
    bool nodelay = true;
    bool keepalive = true;
  };

  TcpSocket() = default;
  ~TcpSocket();
  TcpSocket(TcpSocket&& other) noexcept;
  TcpSocket& operator=(TcpSocket&& other) noexcept;
  TcpSocket(const TcpSocket&) = delete;
  TcpSocket& operator=(const TcpSocket&) = delete;

  // Resolves `host` itself (used by the probe, where a single connection is
  // fine). Workers use connect_endpoint() with an already-resolved edge so
  // starting N connections never serialises on the name resolver.
  void connect(const std::string& host, uint16_t port, const Options& opts);
  void connect_endpoint(const Endpoint& ep, const Options& opts);
  void shutdown_both() noexcept;
  void close() noexcept;

  ssize_t send_some(const void* data, size_t len);
  void send_all(const void* data, size_t len);
  ssize_t recv_some(void* data, size_t len);

  bool valid() const noexcept { return fd_ >= 0; }
  int fd() const noexcept { return fd_; }

 private:
  int fd_ = -1;
};

}  // namespace mtdl
