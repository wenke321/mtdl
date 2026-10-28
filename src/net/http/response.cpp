#include "mtdl/net/http/response.hpp"

#include <cstdlib>

#include "mtdl/net/buffered.hpp"
#include "mtdl/util/error.hpp"
#include "mtdl/util/strings.hpp"

namespace mtdl {

ResponseHead ResponseHead::read(BufferedReader& in) {
  ResponseHead head;
  std::string status_line = in.read_line();
  if (!str::starts_with(status_line, "HTTP/"))  // tolerate a stale keep-alive close
    fail(ErrKind::Protocol, "bad status line: " + status_line);

  auto sp1 = status_line.find(' ');
  if (sp1 == std::string::npos) fail(ErrKind::Protocol, "bad status line: " + status_line);
  auto sp2 = status_line.find(' ', sp1 + 1);
  std::string code = status_line.substr(sp1 + 1, sp2 == std::string::npos ? std::string::npos
                                                                        : sp2 - sp1 - 1);
  head.status_ = std::atoi(code.c_str());
  if (sp2 != std::string::npos) head.reason_ = status_line.substr(sp2 + 1);

  for (;;) {
    std::string line = in.read_line();
    if (line.empty()) break;
    auto colon = line.find(':');
    if (colon == std::string::npos) continue;
    std::string name = str::to_lower(str::trim(line.substr(0, colon)));
    std::string value = str::trim(line.substr(colon + 1));
    head.headers_.emplace_back(std::move(name), std::move(value));
  }
  return head;
}

bool ResponseHead::has(const std::string& name) const {
  const std::string needle = str::to_lower(name);
  for (const auto& h : headers_)
    if (h.first == needle) return true;
  return false;
}

std::string ResponseHead::header(const std::string& name) const {
  const std::string needle = str::to_lower(name);
  for (const auto& h : headers_)
    if (h.first == needle) return h.second;
  return {};
}

bool ResponseHead::chunked() const {
  return str::to_lower(header("transfer-encoding")).find("chunked") != std::string::npos;
}

uint64_t ResponseHead::content_length(bool* ok) const {
  std::string v = header("content-length");
  if (v.empty()) {
    if (ok) *ok = false;
    return 0;
  }
  char* end = nullptr;
  unsigned long long n = std::strtoull(v.c_str(), &end, 10);
  if (end == v.c_str()) {
    if (ok) *ok = false;
    return 0;
  }
  if (ok) *ok = true;
  return static_cast<uint64_t>(n);
}

bool ResponseHead::content_range(uint64_t* first, uint64_t* last, uint64_t* total) const {
  std::string v = header("content-range");  // "bytes 0-0/162399360"
  if (v.empty()) return false;
  auto slash = v.find('/');
  auto dash = v.find('-');
  auto space = v.find(' ');
  if (slash == std::string::npos || dash == std::string::npos || space == std::string::npos)
    return false;
  if (first) *first = std::strtoull(v.c_str() + space + 1, nullptr, 10);
  if (last) *last = std::strtoull(v.c_str() + dash + 1, nullptr, 10);
  if (total) {
    std::string t = v.substr(slash + 1);
    if (t == "*") return false;
    *total = std::strtoull(t.c_str(), nullptr, 10);
  }
  return true;
}

bool ResponseHead::keep_alive() const {
  // HTTP/1.1 defaults to persistent connections; only "close" opts out.
  std::string conn = str::to_lower(header("connection"));
  return conn.find("close") == std::string::npos;
}

std::string ResponseHead::debug_string() const {
  std::string out = std::to_string(status_) + " " + reason_;
  for (const auto& h : headers_) out += "\n  " + h.first + ": " + h.second;
  return out;
}

}  // namespace mtdl
