#pragma once

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>

namespace timeutil {

// Returns std::tm for local time corresponding to the given time_t in a
// thread-safe way across platforms.
inline std::tm LocalTime(const std::time_t &tt) {
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &tt);
#else
  localtime_r(&tt, &tm);
#endif
  return tm;
}

// Formats given std::tm using the provided strftime-like format string.
inline std::string FormatTm(const std::tm &tm, const char *fmt) {
  std::ostringstream oss;
  oss << std::put_time(&tm, fmt);
  return oss.str();
}

// Formats current local time using the provided format string.
inline std::string NowLocalFormatted(const char *fmt) {
  auto now = std::chrono::system_clock::now();
  std::time_t tt = std::chrono::system_clock::to_time_t(now);
  std::tm tm = LocalTime(tt);
  return FormatTm(tm, fmt);
}

// Produces a compact timestamp suitable for filenames: YYYYMMDD_HHMMSS
inline std::string TimestampForFile() {
  return NowLocalFormatted("%Y%m%d_%H%M%S");
}

// Produces a human-friendly time for logs: HH:MM:SS
inline std::string ClockTime() { return NowLocalFormatted("%H:%M:%S"); }

} // namespace timeutil
