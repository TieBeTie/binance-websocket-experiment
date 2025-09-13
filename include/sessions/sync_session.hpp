#pragma once

#include "core/isession.hpp"
#include "core/message.hpp"
#include "logging/latency_event.hpp"
#include "net/backoff.hpp"
#include "net/ws_ops.hpp"
#include "util/branch.hpp"
#include "util/latency.hpp"
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <expected>
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
              std::shared_ptr<RawOrderQueue> queue,
              std::shared_ptr<logging::LatencyQueue> latency_queue)
      : index_(index), host_(std::move(host)), port_(std::move(port)),
        target_(std::move(target)), ring_(std::move(queue)),
        latency_queue_(std::move(latency_queue)) {}

  void Start() override {
    jthread_ = std::jthread([this](std::stop_token st) { this->Run(st); });
  }

  ~SyncSession() {
    if (jthread_.joinable()) {
      try {
        jthread_.request_stop();
      } catch (...) {
      }
    }
  }

private:
  void Run(std::stop_token st) {
    retry::Backoff backoff;
    for (;;) {
      if (st.stop_requested()) {
        break;
      }
      net::io_context ioc;
      ssl::context ssl_ctx(ssl::context::tls_client);
      ssl_ctx.set_default_verify_paths();
      ssl_ctx.set_verify_mode(ssl::verify_peer);

      websocket::stream<beast::ssl_stream<beast::tcp_stream>> ws(ioc, ssl_ctx);
      if (st.stop_requested()) {
        break;
      }
      if (!FastConnectSequence(ws, backoff)) {
        continue;
      }

      backoff.Reset();
      beast::error_code ec = ReadLoop(st, ws);
      if (ec && ec != beast::error::timeout &&
          ec != net::error::operation_aborted) {
        std::cerr << "[session " << index_
                  << "] reconnecting after error: " << ec.message() << "\n";
      }
      retry::WaitSync(backoff.Next());
    }
  }

  bool FastConnectSequence(
      websocket::stream<beast::ssl_stream<beast::tcp_stream>> &ws,
      retry::Backoff &backoff) {
    tcp::resolver resolver(ws.get_executor());

    auto st_resolve = wsops::Resolve(resolver, host_, port_);
    if (BRANCH_UNLIKELY(!st_resolve)) {
      OnError("resolve", st_resolve.error());
      retry::WaitSync(backoff.Next());
      return false;
    }

    auto st_connect =
        wsops::Connect(beast::get_lowest_layer(ws).socket(), *st_resolve);
    if (BRANCH_UNLIKELY(!st_connect)) {
      OnError("connect", st_connect.error());
      retry::WaitSync(backoff.Next());
      return false;
    }

    if (auto st = wsops::SetSni(ws.next_layer(), host_); BRANCH_UNLIKELY(!st)) {
      OnError("sni", st.error());
      retry::WaitSync(backoff.Next());
      return false;
    }

    wsops::SetTcpNoDelay(beast::get_lowest_layer(ws));

    auto st_tls = wsops::TlsHandshake(ws.next_layer());
    if (BRANCH_UNLIKELY(!st_tls)) {
      OnError("handshake", st_tls.error());
      retry::WaitSync(backoff.Next());
      return false;
    }

    wsops::ConfigureWebSocket(ws, std::string("webhook-parsing/0.1"));

    auto st_ws = wsops::WsHandshake(ws, host_, target_);
    if (BRANCH_UNLIKELY(!st_ws)) {
      OnError("ws handshake", st_ws.error());
      retry::WaitSync(backoff.Next());
      return false;
    }

    return true;
  }

  beast::error_code
  ReadLoop(std::stop_token st,
           websocket::stream<beast::ssl_stream<beast::tcp_stream>> &ws) {
    for (;;) {
      if (st.stop_requested()) {
        return {};
      }
      RawOrderUpdate slot;
      beast::error_code ec;
      // Короткий дедлайн для регулярной проверки stop_token
      beast::get_lowest_layer(ws).expires_after(std::chrono::milliseconds(200));
      (void)ring_->acquire(slot);
      slot.clear();
      ws.read(slot, ec);
      if (BRANCH_UNLIKELY(ec)) {
        if (BRANCH_UNLIKELY(ec == beast::error::timeout)) {
          if (st.stop_requested()) {
            return {};
          }
          continue;
        }
        return ec;
      }
      const auto now_ms = lat::EpochMillisUtc();
      auto b = slot.data();
      const char *data = static_cast<const char *>(b.data());
      std::size_t len = b.size();
      const std::int64_t event_ms =
          lat::ExtractEventTimestampMs(std::string_view{data, len});
      latency_queue_->push({now_ms, event_ms});
      (void)ring_->publish(std::move(slot));
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
  std::shared_ptr<RawOrderQueue> ring_;
  std::jthread jthread_;
  std::shared_ptr<logging::LatencyQueue> latency_queue_;
};
