#include "mtdl/net/buffered.hpp"

#include <cstring>

#include "mtdl/util/error.hpp"

namespace mtdl {

bool BufferedReader::fill() {
  if (pos_ < buf_.size()) return true;
  buf_.resize(capacity_);
  ssize_t n = src_.read_some(&buf_[0], capacity_);
  if (n <= 0) {
    buf_.clear();
    pos_ = 0;
    return false;
  }
  buf_.resize(static_cast<size_t>(n));
  pos_ = 0;
  return true;
}

std::string BufferedReader::read_line(size_t max_len) {
  std::string line;
  for (;;) {
    if (!fill()) fail(ErrKind::Protocol, "unexpected EOF while reading HTTP head");
    while (pos_ < buf_.size()) {
      char c = buf_[pos_++];
      if (c == '\n') {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        return line;
      }
      line.push_back(c);
      if (line.size() > max_len) fail(ErrKind::Protocol, "HTTP head line too long");
    }
  }
}

ssize_t BufferedReader::read_some(void* out, size_t len) {
  if (pos_ < buf_.size()) {
    size_t avail = buf_.size() - pos_;
    size_t n = avail < len ? avail : len;
    std::memcpy(out, buf_.data() + pos_, n);
    pos_ += n;
    if (pos_ >= buf_.size()) {
      buf_.clear();
      pos_ = 0;
    }
    return static_cast<ssize_t>(n);
  }
  return src_.read_some(out, len);  // zero-copy fast path
}

}  // namespace mtdl
