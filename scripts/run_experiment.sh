#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

# =====================
# Config (override via env or edit here)
# =====================
URL=${URL:-"wss://fstream.binance.com/ws/btcusdt@bookTicker"}
DURATION=${DURATION:-2}            # seconds per run
ASYNC_NS=${ASYNC_NS:-"1 2 3 4"}   # async fanout values
SYNC_NS=${SYNC_NS:-"1 2 3 4"}     # sync fanout values
OUT_DIR=${OUT_DIR:-"latencies"}
SLEEP_BETWEEN=${SLEEP_BETWEEN:-5}  # seconds between runs

mkdir -p "$OUT_DIR"

# Prebuild (warm up compile cache and generate debug symbols)
if command -v g++-13 >/dev/null 2>&1; then
  CXX=g++-13; CC=gcc-13
else
  CXX=g++; CC=gcc
fi
echo "[BUILD] Configure RelWithDebInfo"
# Fresh configure to avoid stale compiler/toolchain cache
rm -rf build
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_COMPILER=${CXX} -DCMAKE_C_COMPILER=${CC}
echo "[BUILD] Build RelWithDebInfo"
cmake --build build -j

# Build (Release) for runs
echo "[BUILD] Configure Release"
rm -rf build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=${CXX} -DCMAKE_C_COMPILER=${CC}
echo "[BUILD] Build Release"
cmake --build build -j


# Async N=1..4
echo "=== async runs (${DURATION}s each) ==="
for N in ${ASYNC_NS}; do
  OUT="${OUT_DIR}/stream_async_N${N}_$(date +%Y%m%d_%H%M%S).ndjson"
  echo "--> async N=${N} -> ${OUT}"
  ./build/webhook_parsing -u "${URL}" -n ${N} -o ${OUT} -m async -t ${DURATION} || true
  sleep ${SLEEP_BETWEEN}
done

# Python baseline (between async and sync)
echo "=== python baseline (${DURATION}s) ==="
WS_URL="${URL}" DURATION_SECONDS=${DURATION} python3 example.py || true


# Sync N=1..4
echo "=== sync runs (${DURATION}s each) ==="
for N in ${SYNC_NS}; do
  OUT="${OUT_DIR}/stream_sync_N${N}_$(date +%Y%m%d_%H%M%S).ndjson"
  echo "--> sync N=${N} -> ${OUT}"
  ./build/webhook_parsing -u "${URL}" -n ${N} -o ${OUT} -m sync -t ${DURATION} || true
  sleep ${SLEEP_BETWEEN}
done

echo "=== DONE ==="
ls -lt "${OUT_DIR}" | head -n 30
