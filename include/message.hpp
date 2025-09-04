#pragma once

#include <boost/lockfree/spsc_queue.hpp>
#include <cstdint>
#include <string>

struct Message {
  int connection_index;
  std::int64_t arrival_epoch_ms;
  std::string payload;
};

static constexpr std::size_t kMessageQueueCapacity = 16384;
using MessageQueue = boost::lockfree::spsc_queue<
    Message, boost::lockfree::capacity<kMessageQueueCapacity>>;