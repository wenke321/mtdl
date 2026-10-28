#pragma once
#include <cstdint>
#include <string>

namespace mtdl {

struct Url {
  std::string scheme;  // "https" | "http"
  std::string host;    // hostname or IP literal
  std::string port;    // textual, empty when the scheme default applies
  std::string target;  // path + optional query, always starts with '/'

  bool is_tls() const { return scheme == "https"; }
  uint16_t port_num() const;
  std::string authority() const;  // host[:port], port dropped when default
  std::string str() const;

  static Url parse(const std::string& text);
};

}  // namespace mtdl
