#pragma once

#include "latency.hpp"
#include "logger.hpp"
#include "message.hpp"
#include "sessions/isession.hpp"
#include <atomic>
#include <boost/asio.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <ctime>
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
    std::size_t backoff_ms = 200;
    for (;;) {
      beast::error_code ec;
      try {
        tcp::resolver resolver(ioc_);
        auto results = resolver.async_resolve(host_, port_, yield[ec]);
        if (ec) {
          std::cerr << "[async_session " << index_
                    << "] resolve error: " << ec.message() << "\n";
          throw beast::system_error{ec};
        }

        websocket::stream<beast::ssl_stream<tcp::socket>> ws(ioc_, ssl_ctx_);

        // TCP connect
        net::async_connect(beast::get_lowest_layer(ws), results, yield[ec]);
        if (ec) {
          std::cerr << "[async_session " << index_
                    << "] connect error: " << ec.message() << "\n";
          throw beast::system_error{ec};
        }

        // SNI
        if (!SSL_set_tlsext_host_name(ws.next_layer().native_handle(),
                                      host_.c_str())) {
          beast::error_code ssl_ec(static_cast<int>(::ERR_get_error()),
                                   net::error::get_ssl_category());
          throw beast::system_error{ssl_ec};
        }

        // TCP_NODELAY
        beast::get_lowest_layer(ws).set_option(net::ip::tcp::no_delay(true),
                                               ec);
        ec.assign(0, ec.category());

        // TLS handshake
        ws.next_layer().async_handshake(ssl::stream_base::client, yield[ec]);
        if (ec) {
          std::cerr << "[async_session " << index_
                    << "] handshake error: " << ec.message() << "\n";
          throw beast::system_error{ec};
        }

        // Disable permessage-deflate
        websocket::permessage_deflate pmd;
        pmd.client_enable = false;
        pmd.server_enable = false;
        ws.set_option(pmd);

        // Decorate UA
        ws.set_option(
            websocket::stream_base::decorator([](websocket::request_type &req) {
              req.set(beast::http::field::user_agent,
                      std::string("webhook-parsing/async/0.1"));
            }));

        // WS handshake
        ws.async_handshake(host_, target_, yield[ec]);
        if (ec)
          throw beast::system_error{ec};

        backoff_ms = 200; // reset after success

        for (;;) {
          beast::flat_buffer buffer;
          ws.async_read(buffer, yield[ec]);
          if (ec)
            throw beast::system_error{ec};
          const auto now_ms = EpochMillisUtc();
          std::string payload = beast::buffers_to_string(buffer.data());
          if (logger_) {
            if (auto t = ExtractEventTimestampMs(payload)) {
              logger_->LogLatency(session_queue_id_, now_ms - *t);
            }
          }
          Message m;
          m.connection_index = index_;
          m.arrival_epoch_ms = now_ms;
          m.payload = std::move(payload);
          queue_->push(m);
        }
      } catch (const std::exception &e) {
        std::cerr << "[async_session " << index_
                  << "] reconnecting after error: " << e.what() << "\n";
        // sleep before reconnect
        net::steady_timer t(ioc_);
        t.expires_after(std::chrono::milliseconds(backoff_ms));
        beast::error_code tec;
        t.async_wait(yield[tec]);
        backoff_ms = std::min<std::size_t>(backoff_ms * 2, 5000);
      }
    }
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
