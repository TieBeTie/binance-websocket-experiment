#pragma once

#include "logger.hpp"
#include "message.hpp"
#include "reactor.hpp"
#include "sessions/async_session.hpp"
#include "sessions/isession.hpp"
#include "sessions/session.hpp"
#include "stream_merger.hpp"
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

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
  std::vector<std::shared_ptr<MessageQueue>> queues;
  queues.reserve(opt.numConnections);
  for (int i = 0; i < opt.numConnections; ++i) {
    queues.push_back(std::make_shared<MessageQueue>());
  }

  std::optional<Reactor> reactor;
  std::vector<std::unique_ptr<ISession>> sessions;
  FileLogger logger;
  if (mode == RunMode::async) {
    reactor.emplace();
    reactor->Start(1, /*pinCpu=*/2);
    sessions.reserve(opt.numConnections);
    for (int i = 0; i < opt.numConnections; ++i) {
      sessions.emplace_back(std::make_unique<AsyncSession>(
          i, reactor->GetIoContext(), reactor->GetSslContext(), opt.host,
          opt.port, opt.target, queues[i], &logger));
    }
  } else {
    sessions.reserve(opt.numConnections);
    for (int i = 0; i < opt.numConnections; ++i) {
      sessions.emplace_back(std::make_unique<Session>(
          i, opt.host, opt.port, opt.target, queues[i], &logger));
    }
  }

  logger.Start(/*pinCpu=*/9);
  for (auto &s : sessions) {
    s->Start();
  }

  std::optional<std::chrono::steady_clock::time_point> deadline;
  if (opt.seconds > 0) {
    deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(opt.seconds);
  }

  StreamMerger merger{queues, opt.outFile};
  if (!merger.open_ok()) {
    return 1;
  }

  merger.Start(/*pinCpu=*/8);

  if (deadline.has_value()) {
    const auto now = std::chrono::steady_clock::now();
    if (deadline > now) {
      std::this_thread::sleep_until(*deadline);
    }
  }

  if (reactor.has_value()) {
    reactor->Stop();
  }

  merger.Join();
  logger.Join();

  return 0;
}
