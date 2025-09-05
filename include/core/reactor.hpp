#pragma once

#include <boost/asio.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/ssl.hpp>
#include <optional>
#include <thread>
#include <vector>
#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif
#include "util/cpu_affinity.hpp"

namespace net = boost::asio;
namespace ssl = net::ssl;

// Reactor
// Threading model:
// - Owns a single io_context shared by all async sessions
// - Runs io_context::run() on N std::jthread workers (typically 1 for low
//   latency); sessions execute as coroutines on these threads
// - Optional CPU pinning for the first worker to improve cache locality
class Reactor {
public:
  Reactor() : ssl_ctx_(ssl::context::tls_client) {
    ssl_ctx_.set_default_verify_paths();
    ssl_ctx_.set_verify_mode(ssl::verify_peer);
  }

  net::io_context &GetIoContext() { return ioc_; }
  ssl::context &GetSslContext() { return ssl_ctx_; }

  void Start(int numThreads = 1, std::optional<int> pinCpu = std::nullopt) {
    if (!work_guard_.has_value()) {
      work_guard_.emplace(ioc_.get_executor());
    }
    threads_.reserve(static_cast<std::size_t>(numThreads));
    for (int i = 0; i < numThreads; ++i) {
      threads_.emplace_back([this, pinCpu] {
#ifdef __linux__
        if (pinCpu.has_value()) {
          CpuAffinity::PinThisThreadToCpu("reactor", *pinCpu);
        } else {
          CpuAffinity::PickAndPin("reactor");
        }
#endif
        ioc_.run();
      });
    }
  }

  void Stop() {
    if (work_guard_.has_value()) {
      work_guard_.reset();
    }
    ioc_.stop();
  }

  ~Reactor() { Stop(); }

private:
  net::io_context ioc_;
  ssl::context ssl_ctx_;
  std::vector<std::jthread> threads_;
  std::optional<net::executor_work_guard<net::io_context::executor_type>>
      work_guard_;
};
