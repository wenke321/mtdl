#include "mtdl/net/http/client.hpp"

#include <algorithm>
#include <cstdlib>

#include "mtdl/net/http/request.hpp"
#include "mtdl/net/http/response.hpp"
#include "mtdl/util/error.hpp"
#include "mtdl/util/log.hpp"
#include "mtdl/util/strings.hpp"

namespace mtdl {

HttpClient::HttpClient(Url url, ClientOptions opts)
    : url_(std::move(url)), opts_(std::move(opts)) {}

HttpClient::~HttpClient() { disconnect(); }

void HttpClient::disconnect() {
  reader_.reset();
  if (stream_) {
    stream_->close();
    stream_.reset();
  }
  head_.reset();
  body_left_ = 0;
  chunked_ = false;
  chunk_done_ = false;
  chunk_left_ = 0;
}

void HttpClient::connect() {
  disconnect();
  TcpSocket::Options sock_opts;
  sock_opts.connect_timeout_ms = opts_.connect_timeout_ms;
  sock_opts.read_timeout_ms = opts_.read_timeout_ms;
  sock_opts.rcvbuf = opts_.rcvbuf;

  TcpSocket sock;
  if (has_pinned_) {
    sock.connect_endpoint(pinned_, sock_opts);
  } else {
    const std::string peer = opts_.connect_ip.empty() ? url_.host : opts_.connect_ip;
    sock.connect(peer, url_.port_num(), sock_opts);
  }

  if (url_.is_tls()) {
    stream_ = std::make_unique<TlsStream>(std::move(sock), url_.host, opts_.tls);
  } else {
    stream_ = std::make_unique<PlainStream>(std::move(sock));
  }
  reader_ = std::make_unique<BufferedReader>(*stream_);
}

void HttpClient::reset_connection() { disconnect(); }

bool HttpClient::reusable() const {
  if (!head_ || !connected()) return false;
  if (!head_->keep_alive()) return false;
  if (!body_complete()) return false;
  return reader_ && reader_->empty();
}

void HttpClient::request_range(uint64_t from, uint64_t to_inclusive) {
  std::string range = "bytes=" + std::to_string(from) + "-";
  if (to_inclusive != kRangeToEnd && to_inclusive >= from)
    range += std::to_string(to_inclusive);
  send_request(range, false);
}

void HttpClient::request_head() { send_request(std::string(), true); }

void HttpClient::send_request(const std::string& range_header, bool head_only) {
  if (!connected()) connect();

  RequestHead req(head_only ? "HEAD" : "GET", url_.target);
  // Host is emitted by RequestHead::serialize().
  if (!opts_.user_agent.empty()) req.set("User-Agent", opts_.user_agent);
  req.set("Accept", "*/*");
  req.set("Accept-Encoding", "identity");
  req.set("Connection", "keep-alive");
  if (!range_header.empty()) req.set("Range", range_header);
  for (const auto& h : opts_.extra_headers) req.set(h.first, h.second);

  const std::string wire = req.serialize(url_.authority());
  stream_->write_all(wire.data(), wire.size());

  head_ = std::make_unique<ResponseHead>(ResponseHead::read(*reader_));
  body_left_ = 0;
  chunked_ = head_->chunked();
  chunk_done_ = false;
  chunk_left_ = 0;

  if (head_only || head_->status() == 204 || head_->status() == 304) {
    body_left_ = 0;
    chunk_done_ = true;
    return;
  }
  if (chunked_) return;

  bool has_len = false;
  body_left_ = head_->content_length(&has_len);
  if (!has_len) body_left_ = 0;  // no body framing: treat as empty
}

ssize_t HttpClient::read_some(void* buf, size_t len) {
  if (len == 0) return 0;
  if (chunked_) return read_chunked(buf, len);
  if (body_left_ == 0) return 0;

  const size_t want = static_cast<size_t>(std::min<uint64_t>(len, body_left_));
  ssize_t n = reader_->read_some(buf, want);
  if (n <= 0) fail(ErrKind::Protocol, "connection closed before body was complete");
  body_left_ -= static_cast<uint64_t>(n);
  return n;
}

ssize_t HttpClient::read_chunked(void* buf, size_t len) {
  if (chunk_done_) return 0;
  if (chunk_left_ == 0) {
    std::string line = reader_->read_line();
    auto semi = line.find(';');
    if (semi != std::string::npos) line = line.substr(0, semi);
    chunk_left_ = std::strtoull(line.c_str(), nullptr, 16);
    if (chunk_left_ == 0) {
      // Consume optional trailer headers up to the blank line.
      for (;;) {
        std::string trailer = reader_->read_line();
        if (trailer.empty()) break;
      }
      chunk_done_ = true;
      return 0;
    }
  }
  const size_t want = static_cast<size_t>(std::min<uint64_t>(len, chunk_left_));
  ssize_t n = reader_->read_some(buf, want);
  if (n <= 0) fail(ErrKind::Protocol, "connection closed inside a chunk");
  chunk_left_ -= static_cast<uint64_t>(n);
  if (chunk_left_ == 0) reader_->read_line();  // CRLF that terminates the chunk
  return n;
}

}  // namespace mtdl
