#pragma once

#include "backoff.hpp"
#include "latency.hpp"
#include "logger.hpp"
#include "message.hpp"
#include "sessions/isession.hpp"
#include "ws_ops.hpp"
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <ctime>
#include <expected>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <openssl/err.h>
#include <string>
#include <thread>

namespace net = boost::asio;
namespace ssl = net::ssl;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = net::ip::tcp;

// Session (synchronous)
// Threading model:
// - Each Session owns a dedicated std::jthread and performs blocking I/O
// - One Session is the single producer of its SPSC queue; StreamMerger consumes
//   on its dedicated thread
// - Suitable for comparison with async reactor-based implementation
// - Error handling on hot paths uses std::expected (C++23) instead of
//   exceptions
class SyncSession : public ISession {
public:
  SyncSession(int index, std::string host, std::string port, std::string target,
              std::shared_ptr<MessageQueue> queue, FileLogger *logger)
      : index_(index), host_(std::move(host)), port_(std::move(port)),
        target_(std::move(target)), queue_(std::move(queue)), logger_(logger) {
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
            std::string("latencies/sync_conn_") + std::to_string(index_) + "_" +
            ts.str() + ".lat");
      }
    } catch (...) {
    }
  }

  void Start() override {
    jthread_ = std::jthread([this] { this->Run(); });
  }

private:
  void Run() {
    Backoff backoff;
    for (;;) {
      net::io_context ioc;
      ssl::context ssl_ctx(ssl::context::tls_client);
      ssl_ctx.set_default_verify_paths();
      ssl_ctx.set_verify_mode(ssl::verify_peer);

      websocket::stream<beast::ssl_stream<beast::tcp_stream>> ws(ioc, ssl_ctx);
      if (!FastConnectSequence(ws, backoff)) {
        continue;
      }

      backoff.Reset();
      beast::error_code ec = ReadLoop(ws);
      std::cerr << "[session " << index_
                << "] reconnecting after error: " << ec.message() << "\n";
      WaitSync(backoff.Next());
    }
  }

  bool FastConnectSequence(
      websocket::stream<beast::ssl_stream<beast::tcp_stream>> &ws,
      Backoff &backoff) {
    tcp::resolver resolver(ws.get_executor());

    auto st_resolve = Resolve(resolver, host_, port_);
    if (!st_resolve) {
      OnError("resolve", st_resolve.error());
      WaitSync(backoff.Next());
      return false;
    }

    auto st_connect =
        Connect(beast::get_lowest_layer(ws).socket(), *st_resolve);
    if (!st_connect) {
      OnError("connect", st_connect.error());
      WaitSync(backoff.Next());
      return false;
    }

    if (auto st = SetSni(ws.next_layer(), host_); !st) {
      OnError("sni", st.error());
      WaitSync(backoff.Next());
      return false;
    }

    SetTcpNoDelay(beast::get_lowest_layer(ws));

    auto st_tls = TlsHandshake(ws.next_layer());
    if (!st_tls) {
      OnError("handshake", st_tls.error());
      WaitSync(backoff.Next());
      return false;
    }

    ConfigureWebSocket(ws, std::string("webhook-parsing/0.1"));

    auto st_ws = WsHandshake(ws, host_, target_);
    if (!st_ws) {
      OnError("ws handshake", st_ws.error());
      WaitSync(backoff.Next());
      return false;
    }

    return true;
  }

  beast::error_code
  ReadLoop(websocket::stream<beast::ssl_stream<beast::tcp_stream>> &ws) {
    for (;;) {
      beast::flat_buffer buffer;
      beast::error_code ec;
      ws.read(buffer, ec);
      if (ec) {
        return ec;
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
  }

  void OnError(const char *stage, const beast::error_code &ec) {
    std::cerr << "[session " << index_ << "] " << stage
              << " error: " << ec.message() << "\n";
  }

  int index_;
  std::string host_;
  std::string port_;
  std::string target_;
  std::shared_ptr<MessageQueue> queue_;
  std::jthread jthread_;
  FileLogger *logger_;
  uint16_t session_queue_id_ = 0;
};
