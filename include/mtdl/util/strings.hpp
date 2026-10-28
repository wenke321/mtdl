#pragma once
#include <cstdint>
#include <string>

namespace mtdl::str {

std::string trim(std::string s);
std::string to_lower(std::string s);
bool iequals(const std::string& a, const std::string& b);
bool starts_with(const std::string& s, const std::string& prefix);
bool ends_with(const std::string& s, const std::string& suffix);

// "8MiB" / "1024" / "1.5G" -> bytes
uint64_t parse_size(const std::string& text);
// 162399360 -> "154.86 MiB"
std::string format_bytes(uint64_t bytes);
// 26.4 -> "00:26"
std::string format_duration(double seconds);
// rate in MiB/s -> "5.42 MiB/s"
std::string format_speed(double mib_per_second);

}  // namespace mtdl::str
