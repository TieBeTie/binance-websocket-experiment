#pragma once

#include "core/isession.hpp"
#include "core/message.hpp"
#include "logging/latency_event.hpp"
#include "net/backoff.hpp"
#include "net/ws_ops.hpp"
#include "util/branch.hpp"
#include "util/latency.hpp"
#include <boost/asio.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
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
               std::shared_ptr<RawOrderQueue> queue,
               std::shared_ptr<logging::LatencyQueue> latency_queue)
      : index_(index), ioc_(ioc), ssl_ctx_(ssl_ctx), host_(std::move(host)),
        port_(std::move(port)), target_(std::move(target)),
        ring_(std::move(queue)), latency_queue_(std::move(latency_queue)) {}

  void Start() override {
    net::spawn(ioc_, [this](net::yield_context yield) { this->Run(yield); });
  }

private:
  void Run(net::yield_context yield) {
    retry::Backoff backoff;
    for (;;) {
      websocket::stream<beast::ssl_stream<tcp::socket>> ws(ioc_, ssl_ctx_);
      if (!FastConnectSequence(yield, ws, backoff)) {
        continue;
      }
      backoff.Reset();
      beast::error_code ec = ReadLoop(yield, ws);
      std::cerr << "[async_session " << index_
                << "] reconnecting after error: " << ec.message() << "\n";
      retry::WaitAsync(ioc_, yield, backoff.Next());
    }
  }

  // FastConnectSequence: minimal-latency connection setup sequence
  // Resolve → TCP connect → SNI → TCP_NODELAY → TLS handshake → Configure WS →
  // WS handshake
  bool
  FastConnectSequence(net::yield_context yield,
                      websocket::stream<beast::ssl_stream<tcp::socket>> &ws,
                      retry::Backoff &backoff) {
    tcp::resolver resolver(ioc_);
    // Resolve
    auto resultsExp = wsops::AsyncResolve(resolver, host_, port_, yield);
    if (!resultsExp) {
      OnError("resolve", resultsExp.error(), yield, backoff);
      return false;
    }
    // TCP connect
    auto st_connect =
        wsops::AsyncConnect(beast::get_lowest_layer(ws), *resultsExp, yield);
    if (BRANCH_UNLIKELY(!st_connect)) {
      OnError("connect", st_connect.error(), yield, backoff);
      return false;
    }
    // SNI
    if (auto st = wsops::SetSni(ws.next_layer(), host_); BRANCH_UNLIKELY(!st)) {
      OnError("sni", st.error(), yield, backoff);
      return false;
    }

    // TCP_NODELAY
    wsops::SetTcpNoDelay(beast::get_lowest_layer(ws));

    // TLS handshake
    auto st_tls = wsops::AsyncTlsHandshake(ws.next_layer(), yield);
    if (BRANCH_UNLIKELY(!st_tls)) {
      OnError("handshake", st_tls.error(), yield, backoff);
      return false;
    }

    // Configure WS: disable permessage-deflate, set UA decorator
    wsops::ConfigureWebSocket(ws, std::string("webhook-parsing/async/0.1"));
    // WS handshake
    auto st_ws = wsops::AsyncWsHandshake(ws, host_, target_, yield);
    if (BRANCH_UNLIKELY(!st_ws)) {
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
    beast::error_code ec;
    for (;;) {
      RawOrderUpdate slot;
      (void)ring_->acquire(slot);
      slot.clear();
      std::size_t nread = ws.async_read(slot, yield[ec]);
      if (BRANCH_UNLIKELY(ec)) {
        break;
      }
      const auto now_ms = lat::EpochMillisUtc();
      const auto event_ms = lat::ExtractEventTimestampMs(std::string_view{
          static_cast<const char *>(slot.data().data()), nread});
      latency_queue_->push({now_ms, event_ms});
      (void)ring_->publish(std::move(slot));
    }
    return ec;
  }

  void OnError(const char *stage, const beast::error_code &ec,
               net::yield_context yield, retry::Backoff &backoff) {
    std::cerr << "[async_session " << index_ << "] " << stage
              << " error: " << ec.message() << "\n";
    retry::WaitAsync(ioc_, yield, backoff.Next());
  }

  int index_;
  net::io_context &ioc_;
  ssl::context &ssl_ctx_;
  std::string host_;
  std::string port_;
  std::string target_;
  std::shared_ptr<RawOrderQueue> ring_;
  std::shared_ptr<logging::LatencyQueue> latency_queue_;
};
