#pragma once

#include "lockfree/ring.hpp"
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/lockfree/spsc_queue.hpp>

using RawOrderUpdate = boost::beast::flat_buffer;

static constexpr std::size_t kRawOrderQueueCapacity = 16384;
using RawOrderQueue = lockfree::Ring<RawOrderUpdate, kRawOrderQueueCapacity>;