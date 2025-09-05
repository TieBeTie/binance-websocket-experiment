## Architecture: low‑latency market data ingestion

Study goal: receive every message from `wss://fstream.binance.com/ws/btcusdt@bookTicker` with the lowest possible latency, using N parallel connections, merge them into a single ordered stream, persist it, and compute per‑connection latency metrics on receipt.

### Constraints and design choices
- **Platform constraints**: io_uring (proactor) would likely be more efficient for a single‑threaded event loop, but it requires a newer kernel/liburing that is not available in this environment. Therefore, we use **Boost.Asio + Beast (reactor model)** on top of epoll/select. This keeps the code portable and easy to build here.
- **Analysis constraint**: latency is measured per connection right after a complete WebSocket message is received (all frames), as requested, and written to per‑session files to analyze distributions and tails per connection.
- **Isolation of hot paths**: all hot‑path operations avoid allocations and exceptions; common operations are centralized with `std::expected` returns.
- **Thread pinning**: long‑running threads (reactor, merger, logger) are optionally pinned to CPU cores to reduce scheduler noise and stabilize latency.

### Threading model (high‑level)
- **Reactor**: one `io_context` running on 1 worker thread (pinned). All async sessions are coroutines on this thread.
- **AsyncSession**: coroutine on the reactor thread; non‑blocking I/O; produces messages to its SPSC queue and latencies to the logger.
- **SyncSession**: one `std::jthread` per session; blocking I/O; produces to its SPSC queue and the logger. Used for comparison/baseline versus async.
- **StreamMerger**: dedicated `std::jthread` (pinned). Merges N SPSC queues into a single monotonic NDJSON stream using a min‑heap + hold‑back window and dedup by `u`.
- **FileLogger**: dedicated `std::jthread` (pinned). Drains per‑session SPSC rings in round‑robin and writes batches with `writev`.
- **Main**: parses URL and options, starts components, sleeps to deadline, then coordinates shutdown.

---

## Components and why they look this way

### Reactor (`include/core/reactor.hpp`)
- **What it does**: owns a single shared `boost::asio::io_context` and an `ssl::context`. Runs `io_context::run()` on N worker threads (we use 1 for lowest latency). Keeps the `io_context` alive via `executor_work_guard`.
- **Why reactor (not proactor/io_uring)**: io_uring can deliver lower overhead and fewer syscalls, but it requires a newer kernel/liburing that we cannot install here. Asio’s coroutine‑based reactor is wide‑spread, robust, and matches our portability constraint.
- **Why a single worker**: single threaded reactor maximizes cache locality, eliminates cross‑thread handler hops, and avoids contention in the presence of very small messages. Parallelism is achieved by multiple connections (K), not by multiple reactor threads.
- **Why CPU pinning**: optional pin of the worker reduces scheduling jitter; implemented via `CpuAffinity::PickAndPin("reactor")`.

### AsyncSession (`include/sessions/async_session.hpp`)
- **What it does**: a coroutine that continuously reconnects with exponential backoff, reads WebSocket messages, computes per‑message latency and writes it into a per‑session SPSC queue handled by the logger; the raw payload is pushed into the merger’s SPSC queue.
- **Error handling**: hot paths use `std::expected` in helpers (`wsops`) and reconnection waits are implemented by `retry::WaitAsync`.
- **Why coroutine**: minimal overhead and natural sequencing without explicit state machines, while staying on the single reactor thread.
- **Per‑message latency**: measured as `now_ms - event_timestamp_ms`, where event time is extracted from fields `T` or `E`.

#### FastConnectSequence
The connection setup is ordered to minimize handshake latency and avoid feature negotiation overhead:
1) Resolve DNS → 2) TCP connect → 3) Set SNI → 4) Set `TCP_NODELAY` → 5) TLS handshake → 6) Configure WebSocket (disable permessage‑deflate, set UA) → 7) WebSocket handshake.
This is implemented via `wsops::*` helpers and used verbatim in both async and sync sessions. PMD is disabled to avoid compression stalls on small market‑data frames. `TCP_NODELAY` is set to reduce Nagle‑related delays.

### SyncSession (`include/sessions/sync_session.hpp`)
- **What it does**: one `std::jthread` per session performs blocking I/O. Used as a baseline for comparison against the coroutine design. The thread cooperatively checks a `stop_token` using short read deadlines (200ms) to exit quickly on shutdown.
- **Why keep sync**: gives an apples‑to‑apples comparison with the async pipeline; in some environments a blocking thread per connection is competitive and simpler to reason about.
- **Stability**: reconnection waits use `retry::WaitSync`; the class logs only meaningful errors (suppresses “Success”/timeout spam).

### StreamMerger (`include/merge/stream_merger.hpp`)
- **What it does**: merges N producer queues into a single NDJSON file while maintaining strict monotonic order by update id `u`. It uses:
  - a min‑heap ordered by `u`;
  - a small time‑based hold‑back window (20ms) to absorb minor out‑of‑order arrivals;
  - deduplication with a first‑wins policy (`u <= last_emitted_u_` are dropped).
- **Why a min‑heap + hold‑back**: we want lowest end‑to‑end latency with bounded reordering. The window is small to emit promptly while still cleaning up small out‑of‑order bursts caused by network/TLS framing.
- **Why a single coalesced write**: messages ready in one flush are concatenated into a single buffer and written with one `write(2)` to reduce syscall overhead.
- **Shutdown**: on stop request, the thread drains all queues and the heap to ensure no data is lost.

### FileLogger (`include/logging/logger.hpp`)
- **What it does**: per‑session SPSC ring buffers collect pre‑formatted latency lines. A pinned logger thread drains them round‑robin and writes batches via `writev`.
- **Why per‑session SPSC**: true single‑producer/single‑consumer semantics with zero locks, minimal cache contention, and back‑pressure local to a session.
- **Why batching + `writev`**: reduces syscall count and amortizes kernel overhead without touching the hot read path.
- **Safety during shutdown**: an `alive_` flag prevents late producers from touching freed queues; descriptors are closed after the worker exits.

### CpuAffinity (`include/util/cpu_affinity.hpp`)
- **What it does**: small Linux helper to pick the least busy allowed CPU (based on `/proc/stat`) and pin long‑lived threads. Emits timestamped messages like `[affinity] stream_merger pinned to CPU X`.
- **Why**: pinning the reactor/merger/logger reduces context switches and improves tail latency stability. On non‑Linux it becomes a no‑op.

### URL parsing (`include/net/url.hpp`)
- **What it does**: header‑only parser returning `{scheme, host, port, target}`; used directly in `main` to validate input and keep sessions ignorant of parsing details.
- **Why header‑only**: avoids another translation unit and keeps CLI setup self‑contained.

### Shared helpers
- **`include/net/ws_ops.hpp`**: shared resolve/connect/handshake helpers for TLS/WS with `std::expected` status. Also sets SNI, disables PMD, and applies `TCP_NODELAY`.
- **`include/net/backoff.hpp`**: tiny exponential backoff state with sync/async wait helpers (`steady_timer` for async).
- **`include/util/latency.hpp`**: `EpochMillisUtc()` and timestamp extraction from payloads (`T` then fallback to `E`).
- **`include/io/file_writer.hpp`**: robust `WriteAll`/`WritevAll` wrappers to handle partial writes and `EINTR`.

### Message (`include/core/message.hpp`)
- **What it is**: a small DTO `{arrival_epoch_ms, payload}` and an SPSC `MessageQueue` type alias used across sessions and the merger.
- **Why core**: shared by `sessions`, `merge`, and `runner`; kept dependency‑free for reuse.

### Runner and shutdown (`include/core/runner.hpp`)
- **What it does**: wires queues, optional reactor, sessions, logger, and merger; starts them, sleeps to the configured deadline, then stops components.
- **Current shutdown order (matches code)**:
  1) Stop reactor (for async, unwinds coroutines).
  2) Destroy sessions (`sessions.clear()`; sync threads request stop in destructor).
  3) Join merger (drains remaining data and exits).
  4) Join logger (drains remaining latencies and exits).
- **Why this order**: sessions should stop producing before merger/logger finish draining. Reactor is stopped first so async sessions cease I/O; sync sessions honor `stop_token` and exit quickly due to short read deadlines.

### Experiments and artifacts
- **Scripts**: `scripts/run_experiment.sh` builds in RelWithDebInfo and Release, then runs async K∈{1,2,3,4}, Python baseline, and sync K∈{1,2,3,4}. Output:
  - per‑session `.lat` files under `latencies/` (from `FileLogger`),
  - merged NDJSON streams `stream_{mode}_N{K}_timestamp.ndjson` (from `StreamMerger`).
- **Notebook**: `charts/latency_charts.ipynb` discovers files, trims warm‑up, caps samples, and plots distributions (box plots, CCDF, tail thickness). Conclusions accompany each chart.

### What we would change without current constraints
- If a newer kernel and `liburing` were available, we would consider an **io_uring proactor** for the reactor to reduce syscall overhead and improve tail behaviour, keeping the same SPSC/merger/logger architecture.
- For TLS/WS, we would evaluate persistent connections across runs and TLS session resumption where acceptable.

### Summary
- Multiple connections (K) provide earlier‑of‑K opportunities while the merger preserves correctness (monotonic `u`, dedup). The logger and merger are isolated on dedicated pinned threads with SPSC hand‑off to avoid perturbing receive latency. Asio coroutines give a clean, portable, low‑overhead implementation within the platform limits.