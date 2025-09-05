#pragma once

#include "core/isession.hpp"
#include "core/message.hpp"
#include "core/reactor.hpp"
#include "logging/logger.hpp"
#include "merge/stream_merger.hpp"
#include "sessions/async_session.hpp"
#include "sessions/sync_session.hpp"
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

// Runner composition/threading overview:
// - Reactor: runs io_context on 1 thread (pinned), hosts AsyncSession
// coroutines
// - AsyncSession: coroutines on reactor thread; non-blocking I/O, producer to
// StreamMerger and FileLogger via SPSC queues
// - Session (sync): dedicated jthread per session; blocking I/O, producer to
// StreamMerger and FileLogger via SPSC queues
// - StreamMerger: dedicated jthread; consumes all SPSC, min-heap reorder by `u`
// on small window (20ms)
// - FileLogger: dedicated jthread; drains per-session SPSC rings with writev
// - Main thread: sleeps to deadline, then stops reactor, joins components
struct RunOptions {
  std::string host;
  std::string port;
  std::string target;
  int numConnections = 2;
  std::string outFile;
  int seconds = 0;
};

enum class RunMode { async, sync };

inline int Run(const RunOptions &opt, RunMode mode) {
  // Init
  std::vector<std::shared_ptr<MessageQueue>> queues;
  queues.reserve(opt.numConnections);
  for (int i = 0; i < opt.numConnections; ++i) {
    queues.push_back(std::make_shared<MessageQueue>());
  }
  FileLogger logger;
  std::optional<Reactor> reactor;
  std::vector<std::unique_ptr<ISession>> sessions;
  if (mode == RunMode::async) {
    reactor.emplace();
    reactor->Start(1);
    sessions.reserve(opt.numConnections);
    for (int i = 0; i < opt.numConnections; ++i) {
      sessions.emplace_back(std::make_unique<AsyncSession>(
          i, reactor->GetIoContext(), reactor->GetSslContext(), opt.host,
          opt.port, opt.target, queues[i], &logger));
    }
  } else {
    sessions.reserve(opt.numConnections);
    for (int i = 0; i < opt.numConnections; ++i) {
      sessions.emplace_back(std::make_unique<SyncSession>(
          i, opt.host, opt.port, opt.target, queues[i], &logger));
    }
  }
  // Start
  logger.Start();
  for (auto &s : sessions) {
    s->Start();
  }
  std::optional<std::chrono::steady_clock::time_point> deadline;
  StreamMerger merger{queues, opt.outFile};
  if (!merger.OpenOk()) {
    return 1;
  }
  merger.Start();
  // Wait for deadline
  if (opt.seconds > 0) {
    deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(opt.seconds);
  }
  if (deadline.has_value()) {
    const auto now = std::chrono::steady_clock::now();
    if (deadline > now) {
      std::this_thread::sleep_until(*deadline);
    }
  }
  // Stop
  if (reactor.has_value()) {
    reactor->Stop();
  }
  sessions.clear();
  merger.Join();
  logger.Join();
  return 0;
}
