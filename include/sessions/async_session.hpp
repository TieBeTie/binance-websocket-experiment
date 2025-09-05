#pragma once

#include "backoff.hpp"
#include "latency.hpp"
#include "logger.hpp"
#include "message.hpp"
#include "sessions/isession.hpp"
#include "ws_ops.hpp"
#include <boost/asio.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <ctime>
#include <expected>
#include <iomanip>
#include <iostream>
#include <openssl/err.h>
#include <string>

namespace net = boost::asio;
namespace ssl = net::ssl;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = net::ip::tcp;

// AsyncSession
// Threading model:
// - Executes as a Boost.Asio coroutine (spawn) on the Reactor's io_context
//   thread (reactor thread), i.e., no dedicated OS thread per session.
// - All socket operations are non-blocking and cooperatively yield via `yield`.
// - One session produces into its SPSC queue; the StreamMerger consumes on its
//   own thread.
// - Error handling on hot paths uses std::expected (C++23) instead of
// exceptions
class AsyncSession : public ISession {
public:
  AsyncSession(int index, net::io_context &ioc, ssl::context &ssl_ctx,
               std::string host, std::string port, std::string target,
               std::shared_ptr<MessageQueue> queue, FileLogger *logger)
      : index_(index), ioc_(ioc), ssl_ctx_(ssl_ctx), host_(std::move(host)),
        port_(std::move(port)), target_(std::move(target)),
        queue_(std::move(queue)), logger_(logger) {
    try {
      auto now = std::chrono::system_clock::now();
      std::time_t tt = std::chrono::system_clock::to_time_t(now);
      std::tm tm{};
#if defined(_WIN32)
      localtime_s(&tm, &tt);
#else
      localtime_r(&tt, &tm);
#endif
      std::ostringstream ts;
      ts << std::put_time(&tm, "%Y%m%d_%H%M%S");
      if (logger_) {
        session_queue_id_ = logger_->RegisterSession(
            std::string("latencies/async_conn_") + std::to_string(index_) +
            "_" + ts.str() + ".lat");
      }
    } catch (...) {
    }
  }

  void Start() override {
    net::spawn(ioc_, [this](net::yield_context yield) { this->Run(yield); });
  }

private:
  void Run(net::yield_context yield) {
    Backoff backoff;
    for (;;) {
      websocket::stream<beast::ssl_stream<tcp::socket>> ws(ioc_, ssl_ctx_);
      if (!FastConnectSequence(yield, ws, backoff)) {
        continue;
      }
      backoff.Reset();
      beast::error_code ec = ReadLoop(yield, ws);
      std::cerr << "[async_session " << index_
                << "] reconnecting after error: " << ec.message() << "\n";
      WaitAsync(ioc_, yield, backoff.Next());
    }
  }

  // FastConnectSequence: minimal-latency connection setup sequence
  // Resolve → TCP connect → SNI → TCP_NODELAY → TLS handshake → Configure WS →
  // WS handshake
  bool
  FastConnectSequence(net::yield_context yield,
                      websocket::stream<beast::ssl_stream<tcp::socket>> &ws,
                      Backoff &backoff) {
    tcp::resolver resolver(ioc_);
    // Resolve
    auto resultsExp = AsyncResolve(resolver, host_, port_, yield);
    if (!resultsExp) {
      OnError("resolve", resultsExp.error(), yield, backoff);
      return false;
    }
    // TCP connect
    auto st_connect =
        AsyncConnect(beast::get_lowest_layer(ws), *resultsExp, yield);
    if (!st_connect) {
      OnError("connect", st_connect.error(), yield, backoff);
      return false;
    }
    // SNI
    if (auto st = SetSni(ws.next_layer(), host_); !st) {
      OnError("sni", st.error(), yield, backoff);
      return false;
    }

    // TCP_NODELAY
    SetTcpNoDelay(beast::get_lowest_layer(ws));

    // TLS handshake
    auto st_tls = AsyncTlsHandshake(ws.next_layer(), yield);
    if (!st_tls) {
      OnError("handshake", st_tls.error(), yield, backoff);
      return false;
    }

    // Configure WS: disable permessage-deflate, set UA decorator
    ConfigureWebSocket(ws, std::string("webhook-parsing/async/0.1"));
    // WS handshake
    auto st_ws = AsyncWsHandshake(ws, host_, target_, yield);
    if (!st_ws) {
      OnError("ws handshake", st_ws.error(), yield, backoff);
      return false;
    }

    return true;
  }

  void ConfigureSocket(websocket::stream<beast::ssl_stream<tcp::socket>> &ws) {
    websocket::permessage_deflate pmd;
    pmd.client_enable = false;
    pmd.server_enable = false;
    ws.set_option(pmd);
    ws.set_option(
        websocket::stream_base::decorator([](websocket::request_type &req) {
          req.set(beast::http::field::user_agent,
                  std::string("webhook-parsing/async/0.1"));
        }));
  }

  beast::error_code
  ReadLoop(net::yield_context yield,
           websocket::stream<beast::ssl_stream<tcp::socket>> &ws) {
    beast::flat_buffer buffer;
    beast::error_code ec;
    for (;;) {
      buffer.clear();
      std::size_t nread = ws.async_read(buffer, yield[ec]);
      (void)nread;
      if (ec) {
        break;
      }
      const auto now_ms = EpochMillisUtc();
      std::string payload = beast::buffers_to_string(buffer.data());
      auto t = ExtractEventTimestampMs(payload);
      if (logger_ && t) {
        logger_->LogLatency(session_queue_id_, now_ms - *t);
      }
      Message m;
      m.arrival_epoch_ms = now_ms;
      m.payload = std::move(payload);
      queue_->push(m);
    }
    return ec;
  }

  void OnError(const char *stage, const beast::error_code &ec,
               net::yield_context yield, Backoff &backoff) {
    std::cerr << "[async_session " << index_ << "] " << stage
              << " error: " << ec.message() << "\n";
    WaitAsync(ioc_, yield, backoff.Next());
  }

  int index_;
  net::io_context &ioc_;
  ssl::context &ssl_ctx_;
  std::string host_;
  std::string port_;
  std::string target_;
  std::shared_ptr<MessageQueue> queue_;
  FileLogger *logger_;
  uint16_t session_queue_id_ = 0;
};
