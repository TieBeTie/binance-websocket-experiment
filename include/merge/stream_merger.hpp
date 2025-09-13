#pragma once

#include "core/message.hpp"
#include "util/branch.hpp"
#include <atomic>
#include <charconv>
#include <chrono>
#include <optional>
#include <queue>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif
#include "io/file_writer.hpp"
#include "util/cpu_affinity.hpp"
#include <fcntl.h>
#include <unistd.h>

// StreamMerger merges messages from N SPSC queues into a single NDJSON stream
// with the lowest possible latency subject to correctness:
// - Maintains monotonic order by updateId `u` using a min-heap
// - Uses a small time-based hold-back window to reorder minor out-of-order
// bursts
// - Deduplicates by `u` with a first-wins policy (late duplicates are dropped)
// - Emits batched writes via a single coalesced buffer per flush
class StreamMerger {
public:
  // Construct merger with producer queues and output file path
  StreamMerger(std::vector<std::shared_ptr<RawOrderQueue>> queues,
               std::string out_file)
      : queues_(std::move(queues)) {
    fd_ = ::open(out_file.c_str(), O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC,
                 0644);
  }

  ~StreamMerger() {
    Join();
    if (fd_ != -1) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  // Returns true if output file is opened successfully
  bool OpenOk() const { return fd_ != -1; }

  // Starts the merger worker thread; optionally pins it to a CPU
  void Start(std::optional<int> pinCpu = std::nullopt) {
    worker_ = std::jthread([this, pinCpu] {
#ifdef __linux__
      if (pinCpu.has_value()) {
        CpuAffinity::PinThisThreadToCpu("stream_merger", *pinCpu);
      } else {
        CpuAffinity::PickAndPin("stream_merger");
      }
#endif
      this->Run();
    });
  }

  // Requests graceful stop and joins the worker thread
  void Join() {
    stop_requested_.store(true, std::memory_order_relaxed);
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  // Main run loop: ingest → flush ready; on stop and empty queues → drain all
  void Run() {
    for (;;) {
      IngestQueues();
      FlushReady();
      if (stop_requested_.load(std::memory_order_relaxed) && AllQueuesEmpty()) {
        DrainAll();
        break;
      }
      std::this_thread::yield();
    }
  }

private:
  using Clock = std::chrono::steady_clock;
  // Time-based reordering window. The min-heap ensures order by `u` while
  // this hold-back delays emission briefly to collect potentially earlier `u`s.
  static constexpr std::chrono::milliseconds kHoldback_{20};

  // Buffered entry stored in the reordering heap
  struct BufEntry {
    std::uint64_t u;               // updateId used for ordering/dedup
    Clock::time_point first_seen;  // arrival time at the merger
    std::size_t src;               // source queue index to release back
    boost::beast::flat_buffer buf; // raw NDJSON payload (contiguous)
  };

  // Fast parsing of updateId `u` from the payload
  static std::optional<std::uint64_t> ExtractUpdateId(std::string_view s) {
    std::size_t pos = s.find("\"u\"");
    if (pos == std::string_view::npos) {
      return std::nullopt;
    }
    pos = s.find(':', pos);
    if (pos == std::string_view::npos) {
      return std::nullopt;
    }
    ++pos;
    while (pos < s.size() && static_cast<unsigned char>(s[pos]) <= ' ') {
      ++pos;
    }
    const char *first = s.data() + pos;
    const char *last = s.data() + s.size();
    std::uint64_t value = 0;
    auto [ptr, ec] = std::from_chars(first, last, value);
    if (ec == std::errc() && ptr > first) {
      return value;
    }
    return std::nullopt;
  }

  // Returns true if all producer SPSC queues are currently empty
  bool AllQueuesEmpty() const {
    for (const auto &q : queues_) {
      if (q->ready_size() != 0) {
        return false;
      }
    }
    return true;
  }

  // Ingests messages from all queues, parses `u`, and pushes to the min-heap
  // only if u > last_emitted_u_ (late duplicates dropped on push)
  void IngestQueues() {
    for (std::size_t i = 0; i < queues_.size(); ++i) {
      RawOrderUpdate m;
      while (queues_[i]->consume(m)) {
        auto cb = m.data();
        const char *data = static_cast<const char *>(cb.data());
        std::size_t len = cb.size();
        auto ou = ExtractUpdateId(std::string_view{data, len});
        if (!ou.has_value()) {
          continue;
        }
        const std::uint64_t u = *ou;
        if (u <= last_emitted_u_) {
          continue;
        }
        BufEntry e{u, Clock::now(), i, std::move(m)};
        minheap_.push(std::move(e));
      }
    }
  }

  // Flushes ready entries: pops from the min-heap (which orders by smallest
  // `u`) while entries are older than the hold-back window, coalesces them into
  // one buffer, and writes once. Updates last_emitted_u_. Late duplicates (same
  // `u` still in heap) are dropped on a subsequent iteration when observed.
  void FlushReady() {
    if (BRANCH_UNLIKELY(fd_ == -1)) {
      return;
    }
    const auto now = Clock::now();
    std::uint64_t last_u = last_emitted_u_;
    // Batch up to 64 entries with writev: [payload, "\n"] pairs
    std::vector<BufEntry> batch_entries;
    batch_entries.reserve(64);
    struct iovec iov[128];
    int iov_cnt = 0;
    static const char newline = '\n';

    while (!minheap_.empty()) {
      const BufEntry &top = minheap_.top();
      if (BRANCH_UNLIKELY(top.u <= last_u)) {
        minheap_.pop();
        continue;
      }
      if (BRANCH_UNLIKELY(now - top.first_seen < kHoldback_)) {
        break;
      }
      BufEntry e = std::move(const_cast<BufEntry &>(top));
      minheap_.pop();
      auto b = e.buf.data();
      iov[iov_cnt++] = {(void *)b.data(), b.size()};
      iov[iov_cnt++] = {(void *)&newline, 1};
      last_u = e.u;
      batch_entries.emplace_back(std::move(e));
      if (iov_cnt >= 128) {
        io::WritevAll(fd_, iov, iov_cnt);
        // release consumed buffers back to producer rings
        for (auto &be : batch_entries) {
          queues_[be.src]->release(std::move(be.buf));
        }
        iov_cnt = 0;
        batch_entries.clear();
      }
    }

    if (iov_cnt > 0) {
      io::WritevAll(fd_, iov, iov_cnt);
      for (auto &be : batch_entries) {
        queues_[be.src]->release(std::move(be.buf));
      }
      batch_entries.clear();
      last_emitted_u_ = last_u;
    }
  }

  // Final drain without hold-back: emits remaining entries in min-heap order
  // (monotonic by `u`), skipping any late duplicates
  void DrainAll() {
    if (fd_ == -1) {
      return;
    }
    struct iovec iov[128];
    static const char newline = '\n';
    while (!minheap_.empty()) {
      int iov_cnt = 0;
      std::uint64_t last_u = last_emitted_u_;
      std::vector<BufEntry> batch_entries;
      batch_entries.reserve(64);
      while (!minheap_.empty() && iov_cnt <= 126) {
        BufEntry e = std::move(const_cast<BufEntry &>(minheap_.top()));
        minheap_.pop();
        if (e.u > last_u) {
          auto b = e.buf.data();
          iov[iov_cnt++] = {(void *)b.data(), b.size()};
          iov[iov_cnt++] = {(void *)&newline, 1};
          last_u = e.u;
          batch_entries.emplace_back(std::move(e));
        }
      }
      if (iov_cnt > 0) {
        io::WritevAll(fd_, iov, iov_cnt);
        for (auto &be : batch_entries) {
          queues_[be.src]->release(std::move(be.buf));
        }
        last_emitted_u_ = last_u;
      }
    }
  }

  // Producer queues feeding the merger
  std::vector<std::shared_ptr<RawOrderQueue>> queues_;
  // Output file descriptor (append-only)
  int fd_ = -1;
  // Worker thread handling ingestion and flush
  std::jthread worker_;
  // Stop flag requested by Join()
  std::atomic<bool> stop_requested_{false};

  // Last successfully emitted updateId `u` to ensure monotonic stream
  std::uint64_t last_emitted_u_ = 0;
  // Min-heap comparator: smallest `u` has highest priority (top)
  struct MinCmp {
    bool operator()(const BufEntry &a, const BufEntry &b) const {
      return a.u > b.u;
    }
  };
  // Reordering buffer: min-heap ensures monotonic emission by `u`
  std::priority_queue<BufEntry, std::vector<BufEntry>, MinCmp> minheap_;
};
