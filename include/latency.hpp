#pragma once

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

inline std::int64_t EpochMillisUtc() {
  using clock = std::chrono::system_clock;
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             clock::now().time_since_epoch())
      .count();
}

inline std::optional<std::int64_t> ExtractTTimestampMs(const std::string &s) {
  auto pos = s.find("\"T\"");
  if (pos == std::string::npos)
    return std::nullopt;
  pos = s.find(':', pos);
  if (pos == std::string::npos)
    return std::nullopt;
  ++pos;
  while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos])))
    ++pos;
  std::int64_t value = 0;
  bool neg = false;
  if (pos < s.size() && s[pos] == '-') {
    neg = true;
    ++pos;
  }
  bool any = false;
  for (; pos < s.size(); ++pos) {
    char c = s[pos];
    if (c < '0' || c > '9')
      break;
    any = true;
    value = value * 10 + (c - '0');
  }
  if (!any)
    return std::nullopt;
  return neg ? -value : value;
}

inline std::optional<std::int64_t>
ExtractEventTimestampMs(const std::string &s) {
  if (auto t = ExtractTTimestampMs(s)) {
    return t;
  }
  auto pos = s.find("\"E\"");
  if (pos == std::string::npos) {
    return std::nullopt;
  }
  pos = s.find(':', pos);
  if (pos == std::string::npos) {
    return std::nullopt;
  }
  ++pos;
  while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) {
    ++pos;
  }
  std::int64_t value = 0;
  bool neg = false;
  if (pos < s.size() && s[pos] == '-') {
    neg = true;
    ++pos;
  }
  bool any = false;
  for (; pos < s.size(); ++pos) {
    char c = s[pos];
    if (c < '0' || c > '9') {
      break;
    }
    any = true;
    value = value * 10 + (c - '0');
  }
  if (!any) {
    return std::nullopt;
  }
  return neg ? -value : value;
}
