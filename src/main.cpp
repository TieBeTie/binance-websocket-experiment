#include "core/runner.hpp"
#include "net/url.hpp"
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

struct Options {
  std::string url = "wss://fstream.binance.com/ws/btcusdt@bookTicker";
  int num_connections = 2;
  std::string out_file = "stream.ndjson";
  std::string mode = "async";
  int seconds = 0; // 0 = run indefinitely
};

static Options ParseArgs(int argc, char **argv) {
  Options opt;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if ((a == "-u" || a == "--url") && i + 1 < argc)
      opt.url = argv[++i];
    else if ((a == "-n" || a == "--num") && i + 1 < argc)
      opt.num_connections = std::max(1, std::atoi(argv[++i]));
    else if ((a == "-o" || a == "--out") && i + 1 < argc)
      opt.out_file = argv[++i];
    else if ((a == "-m" || a == "--mode") && i + 1 < argc)
      opt.mode = argv[++i];
    else if ((a == "-t" || a == "--seconds") && i + 1 < argc)
      opt.seconds = std::max(0, std::atoi(argv[++i]));
  }
  return opt;
}

int main(int argc, char **argv) {
  auto opt = ParseArgs(argc, argv);
  auto url = URL::ParseWssUrl(opt.url);
  if (!url) {
    std::cerr << "Invalid URL (expected wss://host[:port]/path): " << opt.url
              << "\n";
    return 1;
  }

  std::cout << "Connecting to " << url->host << ":" << url->port << url->target
            << " with N=" << opt.num_connections << ", output='" << opt.out_file
            << "'\n";

  RunOptions ro{.host = url->host,
                .port = url->port,
                .target = url->target,
                .numConnections = opt.num_connections,
                .outFile = opt.out_file,
                .seconds = opt.seconds};
  if (opt.mode == "async") {
    return Run(ro, RunMode::async);
  } else {
    return Run(ro, RunMode::sync);
  }
}