#pragma once

#include <boost/asio.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <expected>
#include <string>

namespace net = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = net::ip::tcp;

using Status = std::expected<void, beast::error_code>;

inline Status MakeStatus(const beast::error_code &ec) {
  if (ec) {
    return std::unexpected(ec);
  }
  return {};
}

inline std::expected<tcp::resolver::results_type, beast::error_code>
AsyncResolve(tcp::resolver &resolver, const std::string &host,
             const std::string &port, net::yield_context yield) {
  beast::error_code ec;
  auto r = resolver.async_resolve(host, port, yield[ec]);
  if (ec) {
    return std::unexpected(ec);
  }
  return r;
}

inline Status AsyncConnect(tcp::socket &sock,
                           const tcp::resolver::results_type &endpoints,
                           net::yield_context yield) {
  beast::error_code ec;
  auto it = net::async_connect(sock, endpoints, yield[ec]);
  (void)it;
  return MakeStatus(ec);
}

inline Status AsyncTlsHandshake(beast::ssl_stream<tcp::socket> &ssl,
                                net::yield_context yield) {
  beast::error_code ec;
  ssl.async_handshake(net::ssl::stream_base::client, yield[ec]);
  return MakeStatus(ec);
}

inline Status
AsyncWsHandshake(websocket::stream<beast::ssl_stream<tcp::socket>> &ws,
                 const std::string &host, const std::string &target,
                 net::yield_context yield) {
  beast::error_code ec;
  ws.async_handshake(host, target, yield[ec]);
  return MakeStatus(ec);
}

// Sync variants
inline std::expected<tcp::resolver::results_type, beast::error_code>
Resolve(tcp::resolver &resolver, const std::string &host,
        const std::string &port) {
  beast::error_code ec;
  auto r = resolver.resolve(host, port, ec);
  if (ec)
    return std::unexpected(ec);
  return r;
}

inline Status Connect(tcp::socket &sock,
                      const tcp::resolver::results_type &endpoints) {
  beast::error_code ec;
  net::connect(sock, endpoints, ec);
  return MakeStatus(ec);
}

inline Status TlsHandshake(beast::ssl_stream<beast::tcp_stream> &ssl) {
  beast::error_code ec;
  ssl.handshake(net::ssl::stream_base::client, ec);
  return MakeStatus(ec);
}

inline Status
WsHandshake(websocket::stream<beast::ssl_stream<beast::tcp_stream>> &ws,
            const std::string &host, const std::string &target) {
  beast::error_code ec;
  ws.handshake(host, target, ec);
  return MakeStatus(ec);
}

// Shared helpers (async/sync)

template <typename SslLayer>
inline Status SetSni(SslLayer &ssl, const std::string &host) {
  if (!SSL_set_tlsext_host_name(ssl.native_handle(), host.c_str())) {
    beast::error_code ssl_ec(static_cast<int>(::ERR_get_error()),
                             net::error::get_ssl_category());
    return std::unexpected(ssl_ec);
  }
  return {};
}

inline void SetTcpNoDelay(tcp::socket &sock) {
  beast::error_code ec;
  sock.set_option(net::ip::tcp::no_delay(true), ec);
  (void)ec;
}

inline void SetTcpNoDelay(beast::tcp_stream &stream) {
  beast::error_code ec;
  stream.socket().set_option(net::ip::tcp::no_delay(true), ec);
  (void)ec;
}

template <typename WS>
inline void ConfigureWebSocket(WS &ws, const std::string &userAgent,
                               bool disablePmd = true) {
  if (disablePmd) {
    websocket::permessage_deflate pmd;
    pmd.client_enable = false;
    pmd.server_enable = false;
    ws.set_option(pmd);
  }
  ws.set_option(websocket::stream_base::decorator(
      [userAgent](websocket::request_type &req) {
        req.set(beast::http::field::user_agent, userAgent);
      }));
}
