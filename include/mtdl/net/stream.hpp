#pragma once
#include <cstddef>
#include <sys/types.h>

#include "mtdl/net/socket.hpp"

namespace mtdl {

// Transport abstraction: plaintext TCP or TLS. HTTP code only ever sees this.
class ByteStream {
 public:
  virtual ~ByteStream() = default;
  // >0 bytes read, 0 clean EOF, throws mtdl::Error on failure/timeout.
  virtual ssize_t read_some(void* buf, size_t len) = 0;
  virtual void write_all(const void* buf, size_t len) = 0;
  virtual void close() = 0;
  virtual bool alive() const = 0;
};

class PlainStream : public ByteStream {
 public:
  explicit PlainStream(TcpSocket sock) : sock_(std::move(sock)) {}

  ssize_t read_some(void* buf, size_t len) override;
  void write_all(const void* buf, size_t len) override;
  void close() override;
  bool alive() const override { return sock_.valid(); }

 private:
  TcpSocket sock_;
};

}  // namespace mtdl
