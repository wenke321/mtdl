#include "mtdl/net/stream.hpp"

#include "mtdl/util/error.hpp"

namespace mtdl {

ssize_t PlainStream::read_some(void* buf, size_t len) {
  if (!sock_.valid()) return 0;
  return sock_.recv_some(buf, len);
}

void PlainStream::write_all(const void* buf, size_t len) {
  if (!sock_.valid()) mtdl::fail(ErrKind::Network, "write on closed socket");
  sock_.send_all(buf, len);
}

void PlainStream::close() { sock_.close(); }

}  // namespace mtdl
