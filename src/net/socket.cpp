#include "mtdl/net/socket.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "mtdl/net/resolver.hpp"
#include "mtdl/util/error.hpp"
#include "mtdl/util/log.hpp"

namespace mtdl {
namespace {

std::string errno_text(const char* what) {
  return std::string(what) + ": " + std::strerror(errno);
}

void set_common_options(int fd, const TcpSocket::Options& opts) {
  if (opts.rcvbuf > 0) {
    int v = opts.rcvbuf;
    if (::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &v, sizeof v) < 0)
      LOG_DEBUG("SO_RCVBUF failed: " << std::strerror(errno));
  }
  if (opts.nodelay) {
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  }
  if (opts.keepalive) {
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
  }
  timeval tv{};
  tv.tv_sec = opts.read_timeout_ms / 1000;
  tv.tv_usec = (opts.read_timeout_ms % 1000) * 1000;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

// Non-blocking connect with a bounded wait; leaves the fd in blocking mode.
// Returns a connected fd or -1 with `err` set.
int connect_addr(const sockaddr* addr, socklen_t addr_len, const TcpSocket::Options& opts,
                 std::string* err) {
  int fd = ::socket(addr->sa_family, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
  if (fd < 0) {
    *err = errno_text("socket");
    return -1;
  }
  set_common_options(fd, opts);

  const int flags = ::fcntl(fd, F_GETFL, 0);
  ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  int rc = ::connect(fd, addr, addr_len);
  if (rc < 0 && errno == EINPROGRESS) {
    pollfd pfd{fd, POLLOUT, 0};
    int prc;
    do {
      prc = ::poll(&pfd, 1, opts.connect_timeout_ms);
    } while (prc < 0 && errno == EINTR);
    if (prc == 0) {
      *err = "connect timeout";
      ::close(fd);
      return -1;
    }
    int so_err = 0;
    socklen_t len = sizeof so_err;
    ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &len);
    if (so_err != 0) {
      *err = std::string("connect: ") + std::strerror(so_err);
      ::close(fd);
      return -1;
    }
    rc = 0;
  } else if (rc < 0) {
    *err = errno_text("connect");
    ::close(fd);
    return -1;
  }

  ::fcntl(fd, F_SETFL, flags);  // back to blocking; timeouts via SO_RCVTIMEO
  return fd;
}

}  // namespace

TcpSocket::~TcpSocket() { close(); }

TcpSocket::TcpSocket(TcpSocket&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }

TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept {
  if (this != &other) {
    close();
    fd_ = other.fd_;
    other.fd_ = -1;
  }
  return *this;
}

void TcpSocket::connect_endpoint(const Endpoint& ep, const Options& opts) {
  close();
  sockaddr_storage ss{};
  socklen_t len = 0;
  if (!ep.ipv6) {
    auto* sa = reinterpret_cast<sockaddr_in*>(&ss);
    sa->sin_family = AF_INET;
    sa->sin_port = htons(ep.port);
    if (::inet_pton(AF_INET, ep.ip.c_str(), &sa->sin_addr) != 1)
      fail(ErrKind::Network, "bad IPv4 address '" + ep.ip + "'");
    len = sizeof(sockaddr_in);
  } else {
    auto* sa = reinterpret_cast<sockaddr_in6*>(&ss);
    sa->sin6_family = AF_INET6;
    sa->sin6_port = htons(ep.port);
    if (::inet_pton(AF_INET6, ep.ip.c_str(), &sa->sin6_addr) != 1)
      fail(ErrKind::Network, "bad IPv6 address '" + ep.ip + "'");
    len = sizeof(sockaddr_in6);
  }

  std::string err;
  const int fd = connect_addr(reinterpret_cast<const sockaddr*>(&ss), len, opts, &err);
  if (fd < 0)
    fail(ErrKind::Network, "connect to " + ep.str() + " failed: " + err);
  fd_ = fd;
}

void TcpSocket::connect(const std::string& host, uint16_t port, const Options& opts) {
  close();

  // An IP literal needs no resolver.
  Endpoint literal;
  if (Resolver::parse(host, port, &literal)) {
    connect_endpoint(literal, opts);
    return;
  }

  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  addrinfo* res = nullptr;
  const std::string port_text = std::to_string(port);
  const int rc = ::getaddrinfo(host.c_str(), port_text.c_str(), &hints, &res);
  if (rc != 0) fail(ErrKind::Network, "resolve " + host + ": " + ::gai_strerror(rc));

  std::string last_err = "no address";
  for (addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
    std::string err;
    const int fd = connect_addr(ai->ai_addr, ai->ai_addrlen, opts, &err);
    if (fd >= 0) {
      fd_ = fd;
      break;
    }
    last_err = err;
  }
  ::freeaddrinfo(res);
  if (fd_ < 0)
    fail(ErrKind::Network, "connect to " + host + ":" + port_text + " failed: " + last_err);
}

void TcpSocket::shutdown_both() noexcept {
  if (fd_ >= 0) ::shutdown(fd_, SHUT_RDWR);
}

void TcpSocket::close() noexcept {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

ssize_t TcpSocket::send_some(const void* data, size_t len) {
  for (;;) {
    ssize_t n = ::send(fd_, data, len, MSG_NOSIGNAL);
    if (n >= 0) return n;
    if (errno == EINTR) continue;
    fail(ErrKind::Network, errno_text("send"));
  }
}

void TcpSocket::send_all(const void* data, size_t len) {
  const char* p = static_cast<const char*>(data);
  size_t left = len;
  while (left > 0) {
    ssize_t n = send_some(p, left);
    if (n == 0) fail(ErrKind::Network, "send returned 0");
    p += n;
    left -= static_cast<size_t>(n);
  }
}

ssize_t TcpSocket::recv_some(void* data, size_t len) {
  for (;;) {
    ssize_t n = ::recv(fd_, data, len, 0);
    if (n >= 0) return n;
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      fail(ErrKind::Network, "read timeout");
    fail(ErrKind::Network, errno_text("recv"));
  }
}

}  // namespace mtdl
