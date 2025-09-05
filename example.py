import json
import os
import time
import numpy as np
from pathlib import Path
from websocket import create_connection

# Config
DURATION_SECONDS = int(os.getenv("DURATION_SECONDS", "60"))
URL = os.getenv("WS_URL", "wss://fstream.binance.com/ws/btcusdt@bookTicker")

# Ensure latencies dir exists
Path("latencies").mkdir(parents=True, exist_ok=True)

ts = time.strftime("%Y%m%d_%H%M%S", time.localtime())
out_path = Path("latencies") / f"python_{ts}.lat"

ws = create_connection(URL)
latencies = []
count = 0
start = time.time()

with open(out_path, "w", buffering=1) as f:
    while True:
        res = ws.recv()
        now_ms = time.time() * 1000.0
        data = json.loads(res)
        t_ms = data.get("T") or data.get("E")
        if t_ms is None:
            continue
        latency = now_ms - float(t_ms)
        f.write(f"{latency:.3f}\n")
        latencies.append(latency)
        count += 1
        if count % 100 == 0:
            p50 = np.percentile(latencies[-100:], 50)
            p90 = np.percentile(latencies[-100:], 90)
            print(f"p50: {p50:.3f} ms p90: {p90:.3f} ms")
        if (time.time() - start) >= DURATION_SECONDS:
            break

print(f"Wrote {len(latencies)} latencies to {out_path}")
