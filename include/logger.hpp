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
#include "cpu_affinity.hpp"
#include "file_writer.hpp"

// LogEvent: pre-formatted line (ASCII digits + optional '-') ready for write
struct LogEvent {
  uint16_t len;
  char buf[32];
};

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
  static constexpr std::size_t kRingCapacity = 1u << 16;

  FileLogger() = default;
  ~FileLogger() {
    Join();
    CloseAll();
  }

  uint16_t RegisterSession(const std::string &path) {
    int fd =
        ::open(path.c_str(), O_CREAT | O_WRONLY | O_APPEND | O_CLOEXEC, 0644);
    uint16_t id = static_cast<uint16_t>(fds_.size());
    fds_.push_back(fd);
    queues_.emplace_back(std::make_unique<QueueType>());
    return id;
  }

  void LogLatency(uint16_t sessionId, std::int64_t deltaMs) {
    if (sessionId >= queues_.size()) {
      return;
    }
    LogEvent ev;
    ev.len = ItoaFast(deltaMs, ev.buf);
    ev.buf[ev.len++] = '\n';
    queues_[sessionId]->push(ev); // drop if full
  }

  void RunLoop() {
    std::size_t current = 0;
    for (;;) {
      if (!this->running_.load(std::memory_order_relaxed)) {
        break;
      }
      const std::size_t n = queues_.size();
      if (n == 0) {
        std::this_thread::yield();
        continue;
      }
      if (current >= n) {
        current = 0;
      }
      DrainQueue(current);
      ++current;
    }
    for (std::size_t i = 0; i < queues_.size(); ++i) {
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
    if (i >= queues_.size()) {
      return;
    }
    auto &q = *queues_[i];
    int fd = fds_[i];
    if (fd == -1) {
      return;
    }
    LogEvent batch[64];
    struct iovec iov[64];
    int cnt = 0;
    while (q.pop(batch[cnt])) {
      iov[cnt] = {(void *)batch[cnt].buf, batch[cnt].len};
      ++cnt;
      if (cnt == 64) {
        io::WritevAll(fd, iov, cnt);
        cnt = 0;
      }
    }
    if (cnt > 0) {
      io::WritevAll(fd, iov, cnt);
    }
  }

  using QueueType =
      boost::lockfree::spsc_queue<LogEvent,
                                  boost::lockfree::capacity<kRingCapacity>>;
  std::vector<std::unique_ptr<QueueType>> queues_;
  std::vector<int> fds_;
};
