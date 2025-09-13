#pragma once

#include <atomic>
#include <boost/lockfree/spsc_queue.hpp>
#include <cstdint>
#include <fcntl.h>
#include <memory>
#include <optional>
#include <string>
#include <sys/uio.h>
#include <thread>
#include <unistd.h>
#include <vector>
#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif
#include "io/file_writer.hpp"
#include "logging/latency_event.hpp"
#include "util/branch.hpp"
#include "util/cpu_affinity.hpp"

using logging::LatencyEvent;

// LoggerBase
// Threading model:
// - Owns one background std::jthread worker (started via Start)
// - Derived class implements RunLoop() and controls draining strategy
// - Join() stops the worker and waits for clean shutdown
template <typename Derived> class LoggerBase {
public:
  LoggerBase() = default;
  ~LoggerBase() { Join(); }

  void Start(std::optional<int> pinCpu = std::nullopt) {
    if (running_.exchange(true)) {
      return;
    }
    worker_ = std::jthread([this, pinCpu] {
#ifdef __linux__
      if (pinCpu.has_value()) {
        CpuAffinity::PinThisThreadToCpu("file_logger", *pinCpu);
      } else {
        CpuAffinity::PickAndPin("file_logger");
      }
#endif
      static_cast<Derived *>(this)->RunLoop();
    });
  }

  void Join() {
    running_.store(false, std::memory_order_relaxed);
    if (worker_.joinable()) {
      worker_.join();
    }
  }

protected:
  std::jthread worker_;
  std::atomic<bool> running_{false};
};

// FileLogger
// Threading model:
// - Single background thread performs round-robin draining of per-session SPSC
//   queues and writes batched lines via writev
// - Each session is a single producer to its own SPSC; the logger is the single
//   consumer for all queues
class FileLogger : public LoggerBase<FileLogger> {
public:
  FileLogger() = default;

  ~FileLogger() {
    alive_.store(false, std::memory_order_relaxed);
    Join();
    CloseAll();
  }

  // Add a session by attaching an external SPSC queue. Logger does not own
  // the queue storage beyond shared ownership.
  uint16_t AddSession(std::shared_ptr<logging::LatencyQueue> externalQueue,
                      const std::string &path) {
    int fd =
        ::open(path.c_str(), O_CREAT | O_WRONLY | O_APPEND | O_CLOEXEC, 0644);
    uint16_t id = static_cast<uint16_t>(fds_.size());
    fds_.push_back(fd);
    ext_queues_.push_back(std::move(externalQueue));
    return id;
  }

  void RunLoop() {
    std::size_t current = 0;
    for (;;) {
      if (BRANCH_UNLIKELY(!this->running_.load(std::memory_order_relaxed))) {
        break;
      }
      const std::size_t n = ext_queues_.size();
      if (BRANCH_UNLIKELY(n == 0)) {
        std::this_thread::yield();
        continue;
      }
      if (current >= n) {
        current = 0;
      }
      DrainQueue(current);
      ++current;
    }
    const std::size_t n = ext_queues_.size();
    for (std::size_t i = 0; i < n; ++i) {
      DrainQueue(i);
    }
  }

private:
  void CloseAll() {
    for (int &fd : fds_) {
      if (fd != -1) {
        ::close(fd);
        fd = -1;
      }
    }
  }

  static uint16_t ItoaFast(std::int64_t v, char out[32]) {
    char tmp[32];
    int i = 0;
    if (v == 0) {
      out[0] = '0';
      return 1;
    }
    bool neg = v < 0;
    std::uint64_t x = neg ? -v : v;
    while (x) {
      tmp[i++] = char('0' + (x % 10));
      x /= 10;
    }
    int o = 0;
    if (neg)
      out[o++] = '-';
    while (i)
      out[o++] = tmp[--i];
    return static_cast<uint16_t>(o);
  }

  void DrainQueue(std::size_t i) {
    if (BRANCH_UNLIKELY(i >= ext_queues_.size())) {
      return;
    }
    auto &q = *ext_queues_[i];
    int fd = fds_[i];
    if (BRANCH_UNLIKELY(fd == -1)) {
      return;
    }
    LatencyEvent ev;
    struct iovec iov[128];
    char linebuf[128][32];
    uint16_t lens[128];
    int cnt = 0;
    // batch consume to reduce syscalls and atomics
    while (q.pop(ev)) {
      std::int64_t delta = ev.arrival_ms - ev.event_ms;
      lens[cnt] = ItoaFast(std::abs(delta), linebuf[cnt]);
      linebuf[cnt][lens[cnt]++] = '\n';
      iov[cnt] = {(void *)linebuf[cnt], lens[cnt]};
      ++cnt;
      if (cnt == 128) {
        io::WritevAll(fd, iov, cnt);
        cnt = 0;
      }
    }
    if (cnt > 0) {
      io::WritevAll(fd, iov, cnt);
    }
  }

  std::vector<std::shared_ptr<logging::LatencyQueue>> ext_queues_;
  std::vector<int> fds_;
  std::atomic<bool> alive_{true};
};
