Low-latency Binance WebSocket MVP (C++)

Overview

This MVP ingests Binance bookTicker via WSS (WebSocket over TLS), using N parallel connections, per-connection ingress buffers, a single merger, and an append-only file sink with latency metrics.

Build

- Dependencies: CMake (>=3.16), a C++20 compiler, Boost (Beast, System, Thread), OpenSSL
- Ubuntu/WSL example:
  - sudo apt-get update && sudo apt-get install -y build-essential cmake libboost-all-dev libssl-dev
- Configure & build:
  - cmake -S . -B build
  - cmake --build build -j

Run

- Defaults: wss://fstream.binance.com/ws/btcusdt@bookTicker, N=2, output=stream.ndjson
- Examples:
  - ./build/webhook_parsing
  - ./build/webhook_parsing -u wss://fstream.binance.com/ws/btcusdt@bookTicker -n 4 -o out.ndjson

Notes

- Keep connections persistent; metrics (p50/p90/p99) are printed periodically.
- For strict low-latency, run close to Binance regions, set CPU affinity, and monitor tail latencies.


