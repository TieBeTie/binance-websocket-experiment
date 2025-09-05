#pragma once

#include <boost/algorithm/string/predicate.hpp>
#include <optional>
#include <string>

namespace URL {

struct UrlParts {
  std::string scheme;
  std::string host;
  std::string port;
  std::string target;
};

std::optional<UrlParts> ParseWssUrl(const std::string &url) {
  std::string u = url;
  if (!boost::algorithm::istarts_with(u, "wss://")) {
    return std::nullopt;
  }
  std::string rest = u.substr(6);
  auto slash = rest.find('/');
  std::string hostport =
      slash == std::string::npos ? rest : rest.substr(0, slash);
  std::string target = slash == std::string::npos ? "/" : rest.substr(slash);
  std::string host = hostport;
  std::string port = "443";
  auto colon = hostport.find(':');
  if (colon != std::string::npos) {
    host = hostport.substr(0, colon);
    port = hostport.substr(colon + 1);
  }
  return UrlParts{
      .scheme = "wss", .host = host, .port = port, .target = target};
}

}; // namespace URL
