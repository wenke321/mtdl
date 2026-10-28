#pragma once
#include <string>

#include "mtdl/net/socket.hpp"
#include "mtdl/net/stream.hpp"

typedef struct ssl_ctx_st SSL_CTX;
typedef struct ssl_st SSL;

namespace mtdl {

struct TlsOptions {
  bool verify = true;
  std::string ca_file;  // empty -> system default trust store
};

// Process-wide TLS context (thread-safe, initialised once, supports client
// session resumption so reconnects skip a full handshake).
class TlsContext {
 public:
  static TlsContext& instance();
  SSL_CTX* raw() const { return ctx_; }

 private:
  TlsContext();
  ~TlsContext();
  TlsContext(const TlsContext&) = delete;
  TlsContext& operator=(const TlsContext&) = delete;
  SSL_CTX* ctx_ = nullptr;
};

class TlsStream : public ByteStream {
 public:
  TlsStream(TcpSocket sock, const std::string& sni_host, const TlsOptions& opts);
  ~TlsStream() override;

  ssize_t read_some(void* buf, size_t len) override;
  void write_all(const void* buf, size_t len) override;
  void close() override;
  bool alive() const override { return ssl_ != nullptr && sock_.valid(); }

 private:
  void handshake(const std::string& sni_host, const TlsOptions& opts);
  TcpSocket sock_;
  SSL* ssl_ = nullptr;
};

}  // namespace mtdl
