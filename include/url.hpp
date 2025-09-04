#pragma once

#include <optional>
#include <string>

struct UrlParts {
  std::string scheme;
  std::string host;
  std::string port;
  std::string target;
};

std::optional<UrlParts> ParseWssUrl(const std::string &url);
