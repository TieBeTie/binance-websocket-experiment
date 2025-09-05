Low‑latency Binance WebSocket client (C++)

Results: see [charts/latency_charts.ipynb](charts/latency_charts.ipynb).

What this project does

This client ingests Binance `bookTicker` over WSS using K parallel connections, merges messages into a single ordered NDJSON stream, and records per‑connection latency (ms) on receipt. The design targets minimal tail latency while preserving correctness (monotonic order by `u`, deduplication) and provides a Python baseline for comparison.

Quick start

- Requirements: CMake (>=3.16), GCC 13+ or Clang with C++23, Boost (system, context, coroutine, Beast), OpenSSL
- Ubuntu/WSL install:
  - sudo apt-get update && sudo apt-get install -y build-essential cmake libboost-all-dev libssl-dev
- Configure & build (Release):
  - cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
  - cmake --build build -j
- Run examples:
  - ./build/webhook_parsing
  - ./build/webhook_parsing -u wss://fstream.binance.com/ws/btcusdt@bookTicker -n 2 -m async -o latencies/stream_async.ndjson -t 10
  - ./build/webhook_parsing -u wss://fstream.binance.com/ws/btcusdt@bookTicker -n 2 -m sync  -o latencies/stream_sync.ndjson  -t 10

Where to read more

- Task brief: [docs/task.md](docs/task.md)
- Architecture: [docs/architecture.md](docs/architecture.md)
- Python baseline script: [docs/example.py](docs/example.py)
- Results & charts (Jupyter): [charts/latency_charts.ipynb](charts/latency_charts.ipynb)

Outputs

- Per‑connection latencies: latencies/async_conn_*.lat, latencies/sync_conn_*.lat (newline‑delimited ms)
- Merged stream: latencies/stream_{async|sync}_N{K}_YYYYMMDD_HHMMSS.ndjson

Notes

- Long‑running threads (reactor/merger/logger) can be pinned to CPUs on Linux to stabilize tails.
- Latency is measured at receipt (after full message) as now_ms − {T|E}.
