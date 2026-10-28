#include "mtdl/net/url.hpp"

#include <cstdlib>

#include "mtdl/util/error.hpp"
#include "mtdl/util/strings.hpp"

namespace mtdl {
namespace {
constexpr uint16_t kDefaultTlsPort = 443;
constexpr uint16_t kDefaultPort = 80;
}  // namespace

uint16_t Url::port_num() const {
  if (port.empty()) return is_tls() ? kDefaultTlsPort : kDefaultPort;
  long v = std::strtol(port.c_str(), nullptr, 10);
  if (v <= 0 || v > 65535) fail(ErrKind::Config, "bad port in URL: " + port);
  return static_cast<uint16_t>(v);
}

std::string Url::authority() const {
  bool is_default = port.empty() || port_num() == (is_tls() ? kDefaultTlsPort : kDefaultPort);
  return is_default ? host : host + ":" + port;
}

std::string Url::str() const {
  return scheme + "://" + authority() + target;
}

Url Url::parse(const std::string& text) {
  Url u;
  std::string rest = text;
  auto scheme_end = rest.find("://");
  if (scheme_end == std::string::npos) {
    u.scheme = "https";
  } else {
    u.scheme = str::to_lower(rest.substr(0, scheme_end));
    rest = rest.substr(scheme_end + 3);
  }
  if (u.scheme != "https" && u.scheme != "http")
    fail(ErrKind::Config, "unsupported scheme: " + u.scheme);

  auto slash = rest.find('/');
  std::string hostport = (slash == std::string::npos) ? rest : rest.substr(0, slash);
  u.target = (slash == std::string::npos) ? "/" : rest.substr(slash);

  if (hostport.empty()) fail(ErrKind::Config, "URL has no host: " + text);
  // IPv6 literals arrive as [::1]:443
  if (hostport.front() == '[') {
    auto close = hostport.find(']');
    if (close == std::string::npos) fail(ErrKind::Config, "bad IPv6 literal: " + text);
    u.host = hostport.substr(1, close - 1);
    if (close + 1 < hostport.size() && hostport[close + 1] == ':')
      u.port = hostport.substr(close + 2);
  } else {
    auto colon = hostport.rfind(':');
    if (colon == std::string::npos) {
      u.host = hostport;
    } else {
      u.host = hostport.substr(0, colon);
      u.port = hostport.substr(colon + 1);
    }
  }
  if (u.host.empty()) fail(ErrKind::Config, "URL has no host: " + text);
  return u;
}

}  // namespace mtdl
