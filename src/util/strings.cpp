#include "mtdl/util/strings.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

#include "mtdl/util/error.hpp"

namespace mtdl::str {

std::string trim(std::string s) {
  auto not_space = [](unsigned char c) { return !std::isspace(c); };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
  s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
  return s;
}

std::string to_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

bool iequals(const std::string& a, const std::string& b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return std::tolower(static_cast<unsigned char>(x)) ==
                  std::tolower(static_cast<unsigned char>(y));
         });
}

bool starts_with(const std::string& s, const std::string& prefix) {
  return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() &&
         s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

uint64_t parse_size(const std::string& text) {
  std::string s = trim(text);
  if (s.empty()) fail(ErrKind::Config, "empty size value");

  char* end = nullptr;
  double value = std::strtod(s.c_str(), &end);
  if (end == s.c_str() || value < 0) fail(ErrKind::Config, "bad size: " + text);

  std::string unit = to_lower(trim(std::string(end)));
  while (!unit.empty() && (unit.back() == 'b')) unit.pop_back();  // MiB -> mi
  double mult = 1.0;
  if (unit.empty() || unit == "") mult = 1.0;
  else if (unit == "k" || unit == "ki") mult = 1024.0;
  else if (unit == "m" || unit == "mi") mult = 1024.0 * 1024.0;
  else if (unit == "g" || unit == "gi") mult = 1024.0 * 1024.0 * 1024.0;
  else if (unit == "t" || unit == "ti") mult = 1024.0 * 1024.0 * 1024.0 * 1024.0;
  else fail(ErrKind::Config, "unknown size unit: " + text);

  return static_cast<uint64_t>(value * mult);
}

std::string format_bytes(uint64_t bytes) {
  const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
  double v = static_cast<double>(bytes);
  int u = 0;
  while (v >= 1024.0 && u < 4) {
    v /= 1024.0;
    ++u;
  }
  char buf[64];
  std::snprintf(buf, sizeof buf, u == 0 ? "%.0f %s" : "%.2f %s", v, units[u]);
  return buf;
}

std::string format_duration(double seconds) {
  if (seconds < 0 || seconds > 359999.0) return "--:--";
  int total = static_cast<int>(seconds + 0.5);
  char buf[32];
  std::snprintf(buf, sizeof buf, "%02d:%02d", total / 60, total % 60);
  return buf;
}

std::string format_speed(double mib) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.2f MiB/s", mib);
  return buf;
}

}  // namespace mtdl::str
