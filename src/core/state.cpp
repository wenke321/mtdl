#include "mtdl/core/state.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>

#include "mtdl/util/log.hpp"
#include "mtdl/util/strings.hpp"

namespace mtdl {
namespace {

const char* kMagic = "mtdl-state-v2";

std::string to_hex(const std::vector<uint8_t>& bits) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  out.reserve((bits.size() + 3) / 4);
  for (size_t i = 0; i < bits.size(); i += 4) {
    int v = 0;
    for (size_t b = 0; b < 4; ++b) {
      v <<= 1;
      if (i + b < bits.size() && bits[i + b]) v |= 1;
    }
    out.push_back(digits[v]);
  }
  return out;
}

std::vector<uint8_t> from_hex(const std::string& hex, size_t count) {
  std::vector<uint8_t> bits(count, 0);
  for (size_t i = 0; i < hex.size(); ++i) {
    char c = hex[i];
    int v;
    if (c >= '0' && c <= '9') v = c - '0';
    else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
    else break;
    for (int b = 0; b < 4; ++b) {
      const size_t idx = i * 4 + static_cast<size_t>(b);
      if (idx < count && (v & (1 << (3 - b)))) bits[idx] = 1;
    }
  }
  return bits;
}

}  // namespace

DownloadState DownloadState::load(const std::string& path) {
  DownloadState st;
  std::ifstream in(path);
  if (!in) return st;

  std::string magic;
  std::getline(in, magic);
  if (magic != kMagic) return st;

  std::string hex;
  std::string key;
  while (in >> key) {
    if (key == "url") {
      in >> std::ws;
      std::getline(in, st.url_);
    } else if (key == "size") {
      in >> st.size_;
    } else if (key == "block_size") {
      in >> st.block_size_;
    } else if (key == "done") {
      in >> hex;
    } else {
      std::string rest;
      std::getline(in, rest);
    }
  }
  if (st.block_size_ > 0) st.done_ = from_hex(hex, st.blocks());
  LOG_DEBUG("resume state loaded from " << path);
  return st;
}

void DownloadState::init(const std::string& url, uint64_t size, uint64_t block_size) {
  std::lock_guard<std::mutex> lock(*mu_);
  url_ = url;
  size_ = size;
  block_size_ = block_size;
  done_.assign(blocks(), 0);
}

size_t DownloadState::blocks() const {
  if (block_size_ == 0) return 0;
  return static_cast<size_t>((size_ + block_size_ - 1) / block_size_);
}

bool DownloadState::is_block_done(size_t idx) const {
  std::lock_guard<std::mutex> lock(*mu_);
  return idx < done_.size() && done_[idx] != 0;
}

void DownloadState::mark_block_done(size_t idx) {
  std::lock_guard<std::mutex> lock(*mu_);
  if (idx < done_.size()) done_[idx] = 1;
}

size_t DownloadState::done_blocks() const {
  std::lock_guard<std::mutex> lock(*mu_);
  size_t n = 0;
  for (uint8_t b : done_) n += b ? 1 : 0;
  return n;
}

uint64_t DownloadState::bytes_done() const {
  const uint64_t n = done_blocks();
  if (block_size_ == 0) return 0;
  const uint64_t bytes = n * block_size_;
  return bytes > size_ ? size_ : bytes;
}

void DownloadState::save(const std::string& path) const {
  std::lock_guard<std::mutex> lock(*mu_);
  const std::string tmp = path + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out) {
      LOG_WARN("cannot write resume state: " << tmp);
      return;
    }
    out << kMagic << "\n"
        << "url " << url_ << "\n"
        << "size " << size_ << "\n"
        << "block_size " << block_size_ << "\n"
        << "done " << to_hex(done_) << "\n";
  }
  std::rename(tmp.c_str(), path.c_str());  // atomic on POSIX
}

}  // namespace mtdl
