#pragma once
#include <stdexcept>
#include <string>

namespace mtdl {

// Error categories let callers decide whether a failure is worth retrying.
enum class ErrKind { Config, Network, Protocol, Io, Cancelled };

class Error : public std::runtime_error {
 public:
  Error(ErrKind kind, std::string msg)
      : std::runtime_error(std::move(msg)), kind_(kind) {}
  ErrKind kind() const noexcept { return kind_; }

 private:
  ErrKind kind_;
};

[[noreturn]] inline void fail(ErrKind k, const std::string& msg) { throw Error(k, msg); }

}  // namespace mtdl
