#pragma once
#include <sstream>
#include <string>

namespace mtdl::log {

enum class Level { Debug = 0, Info = 1, Warn = 2, Error = 3 };

void set_level(Level lv) noexcept;
Level level() noexcept;
void write(Level lv, const std::string& msg);

}  // namespace mtdl::log

#define MTDL_LOG_AT(lv, expr)                                            \
  do {                                                                   \
    if (static_cast<int>(lv) >= static_cast<int>(::mtdl::log::level())) { \
      std::ostringstream _mtdl_os;                                       \
      _mtdl_os << expr;                                                  \
      ::mtdl::log::write((lv), _mtdl_os.str());                          \
    }                                                                    \
  } while (0)

#define LOG_DEBUG(expr) MTDL_LOG_AT(::mtdl::log::Level::Debug, expr)
#define LOG_INFO(expr) MTDL_LOG_AT(::mtdl::log::Level::Info, expr)
#define LOG_WARN(expr) MTDL_LOG_AT(::mtdl::log::Level::Warn, expr)
#define LOG_ERROR(expr) MTDL_LOG_AT(::mtdl::log::Level::Error, expr)
