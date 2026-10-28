#pragma once
#include <cstddef>
#include <string>
#include <sys/types.h>

#include "mtdl/net/stream.hpp"

namespace mtdl {

// Small read-ahead layer so the HTTP head parser never loses body bytes.
// On the hot path (empty buffer) reads land straight in the caller's buffer.
class BufferedReader {
 public:
  explicit BufferedReader(ByteStream& src, size_t capacity = 1 << 16)
      : src_(src), capacity_(capacity) {}

  std::string read_line(size_t max_len = 8192);
  ssize_t read_some(void* out, size_t len);
  bool empty() const { return pos_ >= buf_.size(); }

 private:
  bool fill();
  ByteStream& src_;
  std::string buf_;
  size_t pos_ = 0;
  size_t capacity_;
};

}  // namespace mtdl
