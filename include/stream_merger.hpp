#pragma once

#include "message.hpp"
#include <atomic>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif
#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

class StreamMerger {
public:
  StreamMerger(std::vector<std::shared_ptr<MessageQueue>> queues,
               std::string out_file)
      : queues_(std::move(queues)),
        out_(out_file, std::ios::out | std::ios::app | std::ios::binary) {}

  bool open_ok() const { return static_cast<bool>(out_); }

  void Start(std::optional<int> pinCpu = std::nullopt) {
    worker_ = std::jthread([this, pinCpu] {
#ifdef __linux__
      if (pinCpu.has_value()) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(*pinCpu, &cpuset);
        pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
      }
#endif
      this->Run();
    });
  }

  void Join() {
    stop_requested_.store(true, std::memory_order_relaxed);
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  void Run() {

    for (;;) {
      bool progressed = true;
      while (progressed) {
        progressed = false;
        for (std::size_t i = 0; i < queues_.size(); ++i) {
          Message m;
          while (queues_[i]->pop(m)) {
            progressed = true;
            out_ << m.payload << '\n';
          }
        }
      }
      out_.flush();

      if (stop_requested_.load(std::memory_order_relaxed) && AllQueuesEmpty()) {
        out_.flush();
        break;
      }
    }
  }

private:
  bool AllQueuesEmpty() const {
    for (const auto &q : queues_) {
      if (q->read_available() != 0) {
        return false;
      }
    }
    return true;
  }
  std::vector<std::shared_ptr<MessageQueue>> queues_;
  std::ofstream out_;
  std::jthread worker_;
  std::atomic<bool> stop_requested_{false};
};
