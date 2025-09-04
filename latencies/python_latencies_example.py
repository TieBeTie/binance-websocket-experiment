import json, time, os
import numpy as np
from websocket import create_connection

os.makedirs('latencies', exist_ok=True)
log_path = os.path.join('latencies', 'python_latencies_5s.log')


ws = create_connection("wss://fstream.binance.com/ws/btcusdt@bookTicker")
latencies = []
latencies_p50 = []
latencies_p90 = []
count = 0
end_time = time.time() + 30.0   

while time.time() < end_time:
    res = ws.recv()
    data = json.loads(res)
    latency = time.time()*1000 - data['T']
    latencies.append(latency)
    count += 1
    if count % 100 == 0:
        latencies_p50.append(np.percentile(latencies[-100:], 50))
        latencies_p90.append(np.percentile(latencies[-100:], 90))


with open(log_path, 'w', encoding='utf-8') as f:
    for latency in latencies:
        f.write(f"Latency: {latency} ms\n")

with open(log_path + "p50p90", 'a', encoding='utf-8') as f:
    for p50, p90 in zip(latencies_p50, latencies_p90):
        f.write(f"p50: {p50} ms p90: {p90} ms\n")