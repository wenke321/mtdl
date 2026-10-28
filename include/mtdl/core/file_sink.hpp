#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace mtdl {

// Positional-write sink: workers write disjoint ranges, so no locking needed.
class FileSink {
 public:
  FileSink() = default;
  ~FileSink();
  FileSink(const FileSink&) = delete;
  FileSink& operator=(const FileSink&) = delete;

  void open(const std::string& path, uint64_t size, bool preallocate);
  void write_at(uint64_t offset, const void* data, size_t len);
  void sync();
  void close() noexcept;
  bool is_open() const { return fd_ >= 0; }

 private:
  int fd_ = -1;
  uint64_t size_ = 0;
};

}  // namespace mtdl
