#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

mkdir -p latencies

# Build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build -j >/dev/null


# Async N=1..4
echo "=== async runs (2s each) ==="
for N in 1 2 3 4; do
  OUT="latencies/stream_async_N${N}_$(date +%Y%m%d_%H%M%S).ndjson"
  echo "--> async N=${N} -> ${OUT}"
  ./build/webhook_parsing -u wss://fstream.binance.com/ws/btcusdt@bookTicker -n ${N} -o ${OUT} -m async -t 2 || true
  sleep 2
done

# Python baseline (2s)
echo "=== python baseline (2s) ==="
DURATION_SECONDS=2 python3 example.py || true


# Sync N=1..4
echo "=== sync runs (2s each) ==="
for N in 1 2 3 4; do
  OUT="latencies/stream_sync_N${N}_$(date +%Y%m%d_%H%M%S).ndjson"
  echo "--> sync N=${N} -> ${OUT}"
  ./build/webhook_parsing -u wss://fstream.binance.com/ws/btcusdt@bookTicker -n ${N} -o ${OUT} -m sync -t 2 || true
  sleep 2
done

echo "=== DONE ==="
ls -lt latencies | head -n 30
