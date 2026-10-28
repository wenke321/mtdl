#include "mtdl/core/file_sink.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "mtdl/util/error.hpp"
#include "mtdl/util/log.hpp"

namespace mtdl {

FileSink::~FileSink() { close(); }

void FileSink::open(const std::string& path, uint64_t size, bool preallocate) {
  close();
  fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
  if (fd_ < 0)
    fail(ErrKind::Io, "cannot open '" + path + "' for writing: " + std::strerror(errno));
  size_ = size;
  if (::ftruncate(fd_, static_cast<off_t>(size)) != 0)
    fail(ErrKind::Io, "ftruncate('" + path + "') failed: " + std::strerror(errno));
  if (preallocate) {
    // Reserve the extents up front so the write path never blocks on allocation.
    int rc = ::posix_fallocate(fd_, 0, static_cast<off_t>(size));
    if (rc != 0 && rc != EOPNOTSUPP && rc != EINVAL)
      LOG_DEBUG("posix_fallocate: " << std::strerror(rc));
  }
}

void FileSink::write_at(uint64_t offset, const void* data, size_t len) {
  const char* p = static_cast<const char*>(data);
  size_t left = len;
  uint64_t pos = offset;
  while (left > 0) {
    ssize_t n = ::pwrite(fd_, p, left, static_cast<off_t>(pos));
    if (n < 0) {
      if (errno == EINTR) continue;
      fail(ErrKind::Io, std::string("pwrite failed: ") + std::strerror(errno));
    }
    if (n == 0) fail(ErrKind::Io, "pwrite wrote 0 bytes");
    p += n;
    left -= static_cast<size_t>(n);
    pos += static_cast<uint64_t>(n);
  }
}

void FileSink::sync() {
  if (fd_ >= 0) ::fsync(fd_);
}

void FileSink::close() noexcept {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

}  // namespace mtdl
