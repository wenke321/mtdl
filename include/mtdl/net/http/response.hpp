#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace mtdl {

class BufferedReader;

// Parsed response head; header names are normalised to lower case.
class ResponseHead {
 public:
  static ResponseHead read(BufferedReader& in);

  int status() const { return status_; }
  const std::string& reason() const { return reason_; }
  bool has(const std::string& name) const;
  std::string header(const std::string& name) const;  // "" when absent
  bool chunked() const;
  // Parses Content-Length; sets *ok to false when absent/invalid.
  uint64_t content_length(bool* ok = nullptr) const;
  // Parses "bytes a-b/total"; returns false when the header is missing/bad.
  bool content_range(uint64_t* first, uint64_t* last, uint64_t* total) const;
  bool keep_alive() const;
  std::string debug_string() const;

 private:
  int status_ = 0;
  std::string reason_;
  std::vector<std::pair<std::string, std::string>> headers_;
};

}  // namespace mtdl
