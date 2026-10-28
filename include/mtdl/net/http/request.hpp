#pragma once
#include <string>
#include <utility>
#include <vector>

namespace mtdl {

// Builds an HTTP/1.1 request head. Header names are stored as given.
class RequestHead {
 public:
  RequestHead() = default;
  RequestHead(std::string method, std::string target)
      : method_(std::move(method)), target_(std::move(target)) {}

  void set_method(std::string m) { method_ = std::move(m); }
  void set_target(std::string t) { target_ = std::move(t); }
  void set(const std::string& name, const std::string& value);
  const std::string& target() const { return target_; }

  std::string serialize(const std::string& host_header) const;

 private:
  std::string method_ = "GET";
  std::string target_ = "/";
  std::vector<std::pair<std::string, std::string>> headers_;
};

}  // namespace mtdl
