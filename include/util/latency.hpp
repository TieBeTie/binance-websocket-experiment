#pragma once

#include "util/branch.hpp"
#include <cctype>
#include <chrono>
#include <cstdint>
#include <string_view>

// namespace lat — small helpers for timestamp extraction and timepoints used in
// latency measurement. Keeps parsing and time utilities in one place.
namespace lat {

inline std::int64_t EpochMillisUtc() {
  using clock = std::chrono::system_clock;
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             clock::now().time_since_epoch())
      .count();
}

inline std::int64_t ExtractEventTimestampMs(std::string_view sv) {
  std::size_t pos = sv.find("\"E\":");
  if (BRANCH_UNLIKELY(pos == std::string_view::npos)) {
    return 0;
  }
  const char *p = sv.data() + pos + 4; // after "E":
  std::int64_t value = 0;
  // p < end is guaranteed by the condition above
  while (BRANCH_LIKELY(*p >= '0' && *p <= '9')) {
    value = value * 10 + (*p - '0');
    ++p;
  }
  return value;
}

} // namespace lat
