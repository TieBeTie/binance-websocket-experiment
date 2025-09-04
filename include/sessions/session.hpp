#pragma once

#include "latency.hpp"
#include "logger.hpp"
#include "message.hpp"
#include "sessions/isession.hpp"
#include <boost/asio.hpp>
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

class Session : public ISession {
public:
  Session(int index, std::string host, std::string port, std::string target,
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
    std::size_t backoff_ms = 200;
    for (;;) {
      try {
        net::io_context ioc;
        ssl::context ssl_ctx(ssl::context::tls_client);
        ssl_ctx.set_default_verify_paths();
        ssl_ctx.set_verify_mode(ssl::verify_peer);

        tcp::resolver resolver(ioc);
        websocket::stream<beast::ssl_stream<beast::tcp_stream>> ws(ioc,
                                                                   ssl_ctx);

        auto results = resolver.resolve(host_, port_);
        beast::get_lowest_layer(ws).connect(results);

        if (!SSL_set_tlsext_host_name(ws.next_layer().native_handle(),
                                      host_.c_str())) {
          beast::error_code ec(static_cast<int>(::ERR_get_error()),
                               net::error::get_ssl_category());
          throw beast::system_error{ec};
        }

        beast::get_lowest_layer(ws).socket().set_option(
            net::ip::tcp::no_delay(true));

        ws.next_layer().handshake(ssl::stream_base::client);

        websocket::permessage_deflate pmd;
        pmd.client_enable = false;
        pmd.server_enable = false;
        ws.set_option(pmd);

        ws.set_option(
            websocket::stream_base::decorator([](websocket::request_type &req) {
              req.set(beast::http::field::user_agent,
                      std::string("webhook-parsing/0.1"));
            }));

        ws.handshake(host_, target_);

        backoff_ms = 200; // reset after success

        for (;;) {
          beast::flat_buffer buffer;
          ws.read(buffer);
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
        std::cerr << "[session " << index_
                  << "] reconnecting after error: " << e.what() << "\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
        backoff_ms = std::min<std::size_t>(backoff_ms * 2, 5000);
      }
    }
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
