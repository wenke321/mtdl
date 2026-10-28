#include "mtdl/net/http/request.hpp"

namespace mtdl {

void RequestHead::set(const std::string& name, const std::string& value) {
  for (auto& h : headers_) {
    if (h.first == name) {
      h.second = value;
      return;
    }
  }
  headers_.emplace_back(name, value);
}

std::string RequestHead::serialize(const std::string& host_header) const {
  std::string out;
  out.reserve(256);
  out += method_;
  out += ' ';
  out += target_;
  out += " HTTP/1.1\r\nHost: ";
  out += host_header;
  out += "\r\n";
  for (const auto& h : headers_) {
    out += h.first;
    out += ": ";
    out += h.second;
    out += "\r\n";
  }
  out += "\r\n";
  return out;
}

}  // namespace mtdl
