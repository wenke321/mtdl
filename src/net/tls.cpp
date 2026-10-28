#include "mtdl/net/tls.hpp"

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <cerrno>
#include <cstring>
#include <map>
#include <mutex>

#include "mtdl/util/error.hpp"
#include "mtdl/util/log.hpp"

namespace mtdl {
namespace {

std::mutex g_session_mu;
std::map<std::string, SSL_SESSION*> g_sessions;

std::string openssl_error_text(const std::string& where) {
  std::string out = where;
  unsigned long e = 0;
  char buf[256];
  while ((e = ERR_get_error()) != 0) {
    ERR_error_string_n(e, buf, sizeof buf);
    out += ": ";
    out += buf;
  }
  return out;
}

// Client session cache: makes reconnects cost ~1 RTT instead of a full handshake.
void store_session(const std::string& host, SSL* ssl) {
  SSL_SESSION* sess = SSL_get1_session(ssl);
  if (sess == nullptr) return;
  std::lock_guard<std::mutex> lock(g_session_mu);
  auto it = g_sessions.find(host);
  if (it != g_sessions.end()) SSL_SESSION_free(it->second);
  g_sessions[host] = sess;
}

// Returns an up-ref'd session the caller must SSL_SESSION_free().
// The reference matters: store_session() runs on other threads and frees the
// cached object, so handing out a borrowed pointer would be a use-after-free.
SSL_SESSION* take_session(const std::string& host) {
  std::lock_guard<std::mutex> lock(g_session_mu);
  auto it = g_sessions.find(host);
  if (it == g_sessions.end()) return nullptr;
  if (SSL_SESSION_up_ref(it->second) != 1) return nullptr;
  return it->second;
}

}  // namespace

TlsContext& TlsContext::instance() {
  static TlsContext ctx;
  return ctx;
}

TlsContext::TlsContext() {
  OPENSSL_init_ssl(0, nullptr);
  ctx_ = SSL_CTX_new(TLS_client_method());
  if (ctx_ == nullptr) fail(ErrKind::Network, openssl_error_text("SSL_CTX_new"));
  SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);
  SSL_CTX_set_options(ctx_, SSL_OP_NO_COMPRESSION);
  // We keep our own tiny session cache (one session per host, shared by every
  // edge), so OpenSSL's internal client cache would only duplicate work.
  SSL_CTX_set_session_cache_mode(ctx_, SSL_SESS_CACHE_CLIENT | SSL_SESS_CACHE_NO_INTERNAL_STORE);
  // RELEASE_BUFFERS keeps per-connection memory small, which matters when
  // dozens of sockets are open at once.
  SSL_CTX_set_mode(ctx_, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER |
                             SSL_MODE_ENABLE_PARTIAL_WRITE |
                             SSL_MODE_RELEASE_BUFFERS);
  if (SSL_CTX_set_default_verify_paths(ctx_) != 1)
    LOG_DEBUG("TLS: system trust store unavailable, use --insecure if needed");
}

TlsContext::~TlsContext() {
  if (ctx_ != nullptr) SSL_CTX_free(ctx_);
}

TlsStream::TlsStream(TcpSocket sock, const std::string& sni_host, const TlsOptions& opts)
    : sock_(std::move(sock)) {
  try {
    handshake(sni_host, opts);
  } catch (...) {
    if (ssl_ != nullptr) {
      SSL_free(ssl_);
      ssl_ = nullptr;
    }
    throw;
  }
}

TlsStream::~TlsStream() {
  if (ssl_ != nullptr) {
    SSL_set_quiet_shutdown(ssl_, 1);  // never block waiting for the peer
    SSL_free(ssl_);
    ssl_ = nullptr;
  }
}

void TlsStream::handshake(const std::string& sni_host, const TlsOptions& opts) {
  SSL* ssl = SSL_new(TlsContext::instance().raw());
  if (ssl == nullptr) fail(ErrKind::Network, openssl_error_text("SSL_new"));
  ssl_ = ssl;

  if (SSL_set_fd(ssl, sock_.fd()) != 1)
    fail(ErrKind::Network, openssl_error_text("SSL_set_fd"));
  SSL_set_tlsext_host_name(ssl, sni_host.c_str());

  if (opts.verify) {
    SSL_set_verify(ssl, SSL_VERIFY_PEER, nullptr);
    if (!opts.ca_file.empty()) {
      if (SSL_CTX_load_verify_locations(TlsContext::instance().raw(), opts.ca_file.c_str(),
                                       nullptr) != 1)
        fail(ErrKind::Config, "cannot load CA file: " + opts.ca_file);
    }
    X509_VERIFY_PARAM* param = SSL_get0_param(ssl);
    X509_VERIFY_PARAM_set_hostflags(param, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
    if (X509_VERIFY_PARAM_set1_host(param, sni_host.c_str(), 0) != 1)
      fail(ErrKind::Network, "cannot pin hostname for TLS verification");
  } else {
    SSL_set_verify(ssl, SSL_VERIFY_NONE, nullptr);
  }

  if (SSL_SESSION* cached = take_session(sni_host)) {
    SSL_set_session(ssl, cached);  // up-refs internally
    SSL_SESSION_free(cached);      // drop our own reference
  }

  ERR_clear_error();
  int rc = SSL_connect(ssl);
  if (rc != 1) {
    long verify = SSL_get_verify_result(ssl);
    std::string msg = openssl_error_text("TLS handshake with " + sni_host);
    if (opts.verify && verify != X509_V_OK)
      msg += std::string(" (certificate: ") + X509_verify_cert_error_string(verify) + ")";
    fail(ErrKind::Network, msg);
  }
  store_session(sni_host, ssl);
  LOG_DEBUG("TLS session established with " << sni_host
            << (SSL_session_reused(ssl) ? " (resumed)" : ""));
}

ssize_t TlsStream::read_some(void* buf, size_t len) {
  if (ssl_ == nullptr || !sock_.valid()) return 0;
  for (;;) {
    ERR_clear_error();
    int n = SSL_read(ssl_, buf, static_cast<int>(len));
    if (n > 0) return n;
    int err = SSL_get_error(ssl_, n);
    switch (err) {
      case SSL_ERROR_WANT_READ:
      case SSL_ERROR_WANT_WRITE:
        continue;
      case SSL_ERROR_ZERO_RETURN:
        return 0;
      case SSL_ERROR_SYSCALL:
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
          fail(ErrKind::Network, "read timeout");
        if (errno == 0) return 0;  // peer closed without close_notify
        fail(ErrKind::Network, std::string("TLS read: ") + std::strerror(errno));
      default:
        fail(ErrKind::Network, openssl_error_text("TLS read"));
    }
  }
}

void TlsStream::write_all(const void* buf, size_t len) {
  const char* p = static_cast<const char*>(buf);
  size_t left = len;
  while (left > 0) {
    ERR_clear_error();
    int n = SSL_write(ssl_, p, static_cast<int>(left));
    if (n <= 0) {
      int err = SSL_get_error(ssl_, n);
      if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) continue;
      if (err == SSL_ERROR_SYSCALL && errno == EINTR) continue;
      fail(ErrKind::Network, openssl_error_text("TLS write"));
    }
    p += n;
    left -= static_cast<size_t>(n);
  }
}

void TlsStream::close() {
  if (ssl_ != nullptr) {
    SSL_set_quiet_shutdown(ssl_, 1);
    SSL_free(ssl_);
    ssl_ = nullptr;
  }
  sock_.close();
}

}  // namespace mtdl
