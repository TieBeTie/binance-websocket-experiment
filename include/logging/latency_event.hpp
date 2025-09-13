#pragma once

#include <boost/lockfree/spsc_queue.hpp>
#include <cstdint>

namespace logging {

struct LatencyEvent {
  std::int64_t arrival_ms;
  std::int64_t event_ms;
};

inline constexpr std::size_t kLatencyRingCapacity = 1u << 16;

using LatencyQueue = boost::lockfree::spsc_queue<
    LatencyEvent, boost::lockfree::capacity<kLatencyRingCapacity>>;

} // namespace logging
