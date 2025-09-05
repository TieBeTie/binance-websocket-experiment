#pragma once

#include <algorithm>
#include <chrono>
#include <iostream>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#include <string.h>
#include <unistd.h>
#endif

class CpuAffinity {
public:
  static bool PinThisThreadToCpu(int cpu) {
#ifdef __linux__
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    int rc = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    return rc == 0;
#else
    (void)cpu;
    return false;
#endif
  }

  // Picks the least busy allowed CPU excluding those in 'exclude', returns
  // nullopt on failure.
  static std::optional<int>
  PickLeastBusyAllowedCpuExcluding(const std::vector<int> &exclude,
                                   unsigned sleep_ms = 200) {
#ifdef __linux__
    cpu_set_t mask;
    if (sched_getaffinity(0, sizeof(mask), &mask) != 0) {
      return std::nullopt;
    }
    std::vector<CpuSample> a, b;
    if (!readProcStat(a)) {
      return std::nullopt;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
    if (!readProcStat(b)) {
      return std::nullopt;
    }
    const size_t n = std::min(a.size(), b.size());
    int best = -1;
    double bestUtil = 1e9;
    for (size_t i = 0; i < n; ++i) {
      if (!CPU_ISSET((int)i, &mask)) {
        continue;
      }
      if (std::find(exclude.begin(), exclude.end(), (int)i) != exclude.end()) {
        continue;
      }
      unsigned long long totalA = a[i].user + a[i].nice + a[i].sys + a[i].idle +
                                  a[i].iowait + a[i].irq + a[i].softirq +
                                  a[i].steal;
      unsigned long long totalB = b[i].user + b[i].nice + b[i].sys + b[i].idle +
                                  b[i].iowait + b[i].irq + b[i].softirq +
                                  b[i].steal;
      unsigned long long totalDelta =
          (totalB > totalA) ? (totalB - totalA) : 1ULL;
      unsigned long long idleA = a[i].idle + a[i].iowait;
      unsigned long long idleB = b[i].idle + b[i].iowait;
      unsigned long long idleDelta = (idleB > idleA) ? (idleB - idleA) : 0ULL;
      double util = 1.0 - (double)idleDelta / (double)totalDelta;
      if (util < bestUtil) {
        bestUtil = util;
        best = (int)i;
      }
    }
    if (best >= 0) {
      return best;
    }
    return std::nullopt;
#else
    (void)exclude;
    (void)sleep_ms;
    return std::nullopt;
#endif
  }

  // Pick least busy CPU not yet used; if none, fallback to round-robin among
  // used; then pin current thread.
  static std::optional<int> PickAndPin(const char *who = nullptr) {
#ifdef __linux__
    int chosen = -1;
    {
      std::lock_guard<std::mutex> lock(m_);
      auto opt = PickLeastBusyAllowedCpuExcluding(used_, 150);
      if (opt.has_value()) {
        chosen = *opt;
        used_.push_back(chosen);
      } else if (!used_.empty()) {
        chosen = used_[rr_idx_ % used_.size()];
        rr_idx_++;
      }
    }
    if (chosen >= 0 && PinThisThreadToCpu(chosen)) {
      std::cout << "[affinity] " << (who ? who : "thread") << " pinned to CPU "
                << chosen << "\n";
      return chosen;
    }
    return std::nullopt;
#else
    (void)who;
    return std::nullopt;
#endif
  }

  static void ResetUsed() {
    std::lock_guard<std::mutex> lock(m_);
    used_.clear();
    rr_idx_ = 0;
  }

private:
#ifdef __linux__
  struct CpuSample {
    unsigned long long user{}, nice{}, sys{}, idle{}, iowait{}, irq{},
        softirq{}, steal{}, guest{}, guest_nice{};
  };

  static bool readProcStat(std::vector<CpuSample> &out) {
    FILE *f = fopen("/proc/stat", "r");
    if (!f) {
      return false;
    }
    out.clear();
    char tag[64];
    while (fscanf(f, "%63s", tag) == 1) {
      if (strncmp(tag, "cpu", 3) != 0) {
        break;
      }
      if (strcmp(tag, "cpu") == 0) {
        char buf[512];
        fgets(buf, sizeof(buf), f);
        continue;
      }
      CpuSample s{};
      int read = fscanf(f, "%llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                        &s.user, &s.nice, &s.sys, &s.idle, &s.iowait, &s.irq,
                        &s.softirq, &s.steal, &s.guest, &s.guest_nice);
      if (read < 4) {
        break;
      }
      out.push_back(s);
    }
    fclose(f);
    return !out.empty();
  }
#endif

  inline static std::mutex m_;
  inline static std::vector<int> used_;
  inline static size_t rr_idx_ = 0;
};
