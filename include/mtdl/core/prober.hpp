#pragma once
#include <cstdint>
#include <string>

#include "mtdl/net/http/client.hpp"

namespace mtdl {

struct ProbeResult {
  uint64_t size = 0;
  bool range_supported = false;
  std::string etag;
  std::string last_modified;
  std::string content_type;
};

// Asks the server for one byte to learn the real size and range support.
class Prober {
 public:
  Prober(Url url, ClientOptions opts) : url_(std::move(url)), opts_(std::move(opts)) {}
  ProbeResult probe();

 private:
  Url url_;
  ClientOptions opts_;
};

}  // namespace mtdl
