#pragma once
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace mtdl {

// Resume metadata: a bitmap over fixed-size blocks of the output file.
// Block granularity keeps interruption losses small (a few hundred KiB) and
// makes the state file independent of how work was split among connections.
class DownloadState {
 public:
  static DownloadState load(const std::string& path);

  bool matches(const std::string& url, uint64_t size) const {
    return size_ == size && url_ == url;
  }
  void init(const std::string& url, uint64_t size, uint64_t block_size);

  uint64_t block_size() const { return block_size_; }
  size_t blocks() const;
  uint64_t size() const { return size_; }

  bool is_block_done(size_t idx) const;
  void mark_block_done(size_t idx);
  size_t done_blocks() const;
  uint64_t bytes_done() const;

  void save(const std::string& path) const;

 private:
  // shared_ptr keeps the class movable while guarding concurrent updates
  mutable std::shared_ptr<std::mutex> mu_ = std::make_shared<std::mutex>();
  std::string url_;
  uint64_t size_ = 0;
  uint64_t block_size_ = 0;
  std::vector<uint8_t> done_;
};

}  // namespace mtdl
