#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "mtdl/net/buffered.hpp"
#include "mtdl/net/endpoint.hpp"
#include "mtdl/net/stream.hpp"
#include "mtdl/net/tls.hpp"
#include "mtdl/net/url.hpp"

namespace mtdl {

class ResponseHead;

struct ClientOptions {
  TlsOptions tls;
  int connect_timeout_ms = 15000;
  int read_timeout_ms = 30000;
  int rcvbuf = 0;
  std::string connect_ip;  // empty -> resolve url.host
  std::string user_agent;
  std::vector<std::pair<std::string, std::string>> extra_headers;
};

// Minimal HTTP/1.1 client: one connection, sequential range requests,
// keep-alive reuse, Content-Length and chunked bodies.
class HttpClient {
 public:
  HttpClient(Url url, ClientOptions opts);
  ~HttpClient();

  void connect();
  void disconnect();

  // Pins this client to one resolved edge. TLS still uses url().host for SNI
  // and certificate verification, so pinning is purely a routing decision.
  void set_endpoint(const Endpoint& ep) {
    pinned_ = ep;
    has_pinned_ = true;
  }
  const Endpoint& pinned_endpoint() const { return pinned_; }
  bool has_pinned_endpoint() const { return has_pinned_; }
  bool connected() const { return stream_ != nullptr && stream_->alive(); }

  // Inclusive end. Use kRangeToEnd for "to end of file".
  static constexpr uint64_t kRangeToEnd = ~static_cast<uint64_t>(0);
  void request_range(uint64_t from, uint64_t to_inclusive);
  void request_head();

  const ResponseHead& head() const { return *head_; }
  // Reads body bytes; returns 0 once the body is complete.
  ssize_t read_some(void* buf, size_t len);
  bool body_complete() const { return body_left_ == 0 && (chunk_done_ || !chunked_); }
  const Url& url() const { return url_; }
  // True when the previous response permits reusing this connection.
  bool reusable() const;

 private:
  void send_request(const std::string& range_header, bool head_only);
  ssize_t read_chunked(void* buf, size_t len);
  void reset_connection();

  Url url_;
  ClientOptions opts_;
  std::unique_ptr<ByteStream> stream_;
  std::unique_ptr<BufferedReader> reader_;
  std::unique_ptr<ResponseHead> head_;
  uint64_t body_left_ = 0;
  bool chunked_ = false;
  bool chunk_done_ = false;
  uint64_t chunk_left_ = 0;
  Endpoint pinned_;
  bool has_pinned_ = false;
};

}  // namespace mtdl
