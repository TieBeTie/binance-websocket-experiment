#pragma once

#include <algorithm>
#include <boost/asio.hpp>
#include <boost/asio/spawn.hpp>
#include <chrono>
#include <cstddef>
#include <thread>

namespace net = boost::asio;

struct Backoff {
  std::size_t current_ms = 200;
  std::size_t max_ms = 5000;

  void Reset() { current_ms = 200; }
  std::size_t Next() {
    std::size_t v = current_ms;
    current_ms = std::min(max_ms, current_ms * 2);
    return v;
  }
};

inline void WaitSync(std::size_t ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

inline void WaitAsync(net::io_context &ioc, net::yield_context yield,
                      std::size_t ms) {
  boost::system::error_code ec;
  net::steady_timer t(ioc);
  t.expires_after(std::chrono::milliseconds(ms));
  t.async_wait(yield[ec]);
  (void)ec;
}
