# OrderPulse

A single-instrument limit order book and matching engine in C++17, built as
a portfolio project focused on the parts of trading infrastructure that are
hardest to fake convincingly: real price-time-priority matching, a real
concurrency boundary between a feed handler and a matching engine, real
latency measurement with high-resolution timers, and a real profiling
session that found a real bottleneck and fixed it — with the numbers to
prove it, before and after.

Nothing in this repo is estimated or hardcoded. Every latency number below
came out of an actual benchmark run on the machine this was built on
(commands to reproduce are in [Reproducing the results](#reproducing-the-results)).

## Contents

- [What it does](#what-it-does)
- [Architecture](#architecture)
- [Concurrency design](#concurrency-design)
- [Latency measurement](#latency-measurement)
- [Benchmark methodology](#benchmark-methodology)
- [Profiling & optimization](#profiling--optimization)
- [Correctness testing](#correctness-testing)
- [Web demo](#web-demo)
- [Build & run](#build--run)
- [Honest limitations](#honest-limitations)

## What it does

OrderPulse accepts limit and market orders, matches them against resting
liquidity by price-time priority (best price first, oldest order at that
price first), supports partial fills, cancels, and modifies, and processes
a simulated market-data event stream through a two-thread pipeline: a feed
handler thread generating events and a matching thread consuming them,
connected by a lock-free queue. A benchmark harness drives this pipeline
with 2,000,000 events and reports real p50/p90/p99/p99.9/p99.99 processing
latency, measured with `RDTSCP`.

## Architecture

### Data structures

- **`Order`** ([order.hpp](include/orderpulse/order.hpp)): id, side, type,
  price, remaining quantity, plus intrusive `prev`/`next` links. Orders
  live in an **`OrderPool`** ([order_pool.hpp](include/orderpulse/order_pool.hpp)) —
  a fixed-capacity `std::vector<Order>` with a freelist of indices.
  `acquire()`/`release()` never call `new`/`delete`: the matching engine's
  hot path does zero heap allocation for orders.
- **`PriceLevel`** ([price_level.hpp](include/orderpulse/price_level.hpp)):
  all resting orders at one price, as an intrusive doubly-linked FIFO
  (`head` = oldest = fills first). `pushTail`/`spliceOut` are O(1), no
  allocation. Deliberately doesn't own quantity bookkeeping across a
  partial fill vs. a full removal — those decrement different amounts, and
  the header explains why that ambiguity is a real bug class worth
  designing away.
- **`PriceLadder`** ([price_ladder.hpp](include/orderpulse/price_ladder.hpp)):
  one side of the book (bids or asks) as a flat, pre-allocated array of
  `PriceLevel`, indexed directly by `price - minPrice`. This is the
  *second* version of the book's representation — see
  [Profiling & optimization](#profiling--optimization) for what the first
  version was, why it changed, and the real numbers behind that decision.
  Best-price tracking is a maintained cursor: an insert can only ever
  improve it (O(1) comparison), and only removing the *current* best
  requires a walk outward — bounded in practice by how many consecutive
  empty ticks separate the old and new best under realistic order flow.
- **`OrderBook`** ([order_book.hpp](include/orderpulse/order_book.hpp)):
  ties it together. `addOrder`/`cancelOrder`/`modifyOrder`, plus
  introspection (`bestBid`/`bestAsk`/`qtyAtPrice`/`totalRestingQty`/
  `totalOrderCount`) used by both the tests and the benchmark's sanity
  checks. Single-threaded by design — see
  [Concurrency design](#concurrency-design) for why that's the right
  boundary, not a limitation.

### Matching algorithm

A marketable order (a `Market`, or a `Limit` that crosses) sweeps the
opposite side's `PriceLadder` from best price outward: at each level, fill
from the head of its FIFO until either the incoming order is filled or the
level is exhausted, then advance to the next best level. Partial fills
adjust the resting order's quantity in place; full fills splice the order
out and free its pool slot. Any unfilled remainder of a `Limit` rests on
the book; a `Market` order's remainder is dropped (no resting market
orders — matches real exchange behavior).

`cancelOrder` looks up the order by id (O(1) dense index, `OrderId ->
Order*`), unlinks it from its level, and frees its pool slot.
`modifyOrder` implements real exchange semantics: a **quantity decrease at
the same price** keeps queue position (adjusted in place); a **price
change or quantity increase loses time priority** (implemented as
cancel + re-add, which can also match immediately if the new price is
marketable).

### Why C++17, not C++20

Everything used here — structured bindings, `if constexpr`, explicit
memory-order atomics, aligned storage — is C++17. C++20 would add
conveniences (`std::bit_ceil`, `std::span`, `atomic::wait`) but none are
essential, and C++17 keeps the "builds cleanly on a standard Linux box"
requirement easiest to satisfy across toolchains.

## Concurrency design

A feed-simulator thread (producer) and the matching thread (consumer) are
connected by **`SpscQueue<T>`** ([spsc_queue.hpp](include/orderpulse/spsc_queue.hpp)),
a single-producer/single-consumer lock-free ring buffer: power-of-two
capacity (mask instead of modulo), head/tail atomics each pinned to their
own cache line, one atomic load/store per operation on each side.

**Why SPSC, not a mutex-guarded queue or a general MPMC queue:** the real
topology here is exactly one feed handler producing and exactly one
matching core consuming — which is also how a real exchange gateway and
matcher pair is shaped (the matching engine itself is conventionally
single-threaded and deterministic; concurrency lives at the boundary, not
inside the book). SPSC makes both ends wait-free with a single atomic each
and no CAS loop. A mutex would put a lock on the single hottest path in
the system and hand the tail latency to whatever the OS scheduler feels
like doing under contention. MPMC would pay for a kind of concurrency this
system doesn't have.

Events cross the boundary as **`FeedEvent`**
([feed_event.hpp](include/orderpulse/feed_event.hpp)) — a flat,
trivially-copyable struct (Add/Cancel/Modify) copied by value into the
ring. No pointers, no ownership question, no allocation on either side of
the thread boundary.

[tests/test_concurrency.cpp](tests/test_concurrency.cpp) runs this for
real (200k events through actual `std::thread`s) and checks every event is
applied exactly once and the no-lost/no-duplicated-orders invariant holds
throughout — proving the thread split doesn't change any of the
correctness guarantees established for the single-threaded engine.

## Latency measurement

The matching thread timestamps immediately before and after each event's
processing call (`addOrder`/`cancelOrder`/`modifyOrder`) —
matching-engine-isolated latency, not queue wait time (which is reported
separately, see below).

**`RDTSCP`, not plain `RDTSC`** ([bench/latency.hpp](bench/latency.hpp)):
`RDTSCP` is a partial serializing read — it waits for prior instructions
to retire before sampling the counter, which keeps out-of-order execution
from smearing unrelated work across the boundary of what's being timed.
The fully serializing alternative, `CPUID` immediately before `RDTSC`, was
rejected: `CPUID` costs on the order of 20-40 cycles, a large fraction of
a matching operation that itself may take only a few hundred cycles, which
would distort exactly the tail latencies this exists to measure.

TSC frequency is calibrated **once at startup** (comparing a TSC delta
against a `std::chrono::steady_clock` delta over a wall-clock interval),
not per-sample — the hot path records raw cycle deltas into a pre-reserved
`std::vector<uint64_t>` (zero allocation, zero floating point), and
conversion to nanoseconds happens once, after the run, when computing
percentiles ([bench/latency.cpp](bench/latency.cpp)). The benchmark also
checks `/proc/cpuinfo` for `constant_tsc` before trusting cross-core TSC
comparisons, and pins the feed and matching threads to separate cores
(`sched_setaffinity`) to reduce scheduler-induced migration jitter in the
tail.

## Benchmark methodology

[bench/bench_latency.cpp](bench/bench_latency.cpp) wires a
`FeedSimulator` (producer thread) to an `OrderBook` (consumer thread)
through the real `SpscQueue`, over 2,000,000 events (20,000-event warmup
excluded from reported stats — chosen to comfortably outlast the 65,536
event queue-fill transient, see below), and reports two distributions:

- **Processing latency** — the matching-engine-isolated number, the
  primary metric.
- **End-to-end (feed-enqueue to processed)** — includes queue wait time.

The producer is **deliberately unthrottled**: it pushes events as fast as
it can generate them, to stress-test sustained peak throughput rather than
simulate a paced real-time feed. Under a sustained run where the producer
can generate events faster than the consumer drains them, the queue
saturates and stays near-full for most of the run — so end-to-end latency
is dominated by queue backlog, on the order of `queue_capacity x
per-event service time` (65,536 slots x a per-item drain cost in the
hundreds of nanoseconds, once the consumer loop's un-timed overhead around
the measured processing window is included, not just the processing
latency figure itself) — order-of-magnitude single-digit milliseconds,
which is what the measured end-to-end p50 (~10ms, see
[docs/results_optimized.txt](docs/results_optimized.txt)) reflects. This
is a real,
understood saturation effect (the SPSC queue is correctly backpressuring
the producer via failed `tryPush`, not losing data or growing unbounded),
not a defect, and it's a genuinely useful number in its own right: it
demonstrates the pipeline degrades gracefully and boundedly under sustained
overload rather than unboundedly. It is *not* representative of latency
under a realistically-paced feed, where end-to-end would track processing
latency closely.

### Feed simulator

[feed_simulator.hpp](include/orderpulse/feed_simulator.hpp) generates order
flow shaped to match commonly-cited stylized facts about real limit order
books, not uniform random noise:

- **Price offsets from mid are geometric, not uniform** — resting
  liquidity clusters within a few ticks of the touch.
- **Cancels are ~40% of raw event volume by default** — real LOB flow is
  cancel-heavy; most orders get pulled and replaced, not filled.
- **A configurable ~12% of adds are aggressive** (priced to cross,
  occasionally as true `Market` orders) — most flow is passive, a minority
  actually trades.
- **The simulator's own mid drifts** via a small random walk over the run
  instead of sitting static.

The simulator deliberately never reads `OrderBook` state — exactly like a
real feed handler, it only knows what it believes is still live. A
consequence, by design: a `Cancel`/`Modify` can legitimately race against
an order that already filled and gets harmlessly rejected by the book (not
a bug — [tests/test_feed_simulator.cpp](tests/test_feed_simulator.cpp)
runs this against a real book and checks the invariant survives).

## Profiling & optimization

This is the part of the project meant to hold up under interview
questioning, so it's written as what actually happened, including the part
that didn't go as predicted.

### Step 1: the benchmark caught a bug in itself first

Running the very first baseline benchmark produced a suspiciously heavy
tail (p99.99 = 37.6us, max = 9.6ms on a 2M-event run of an operation that
should be sub-microsecond). Before touching the matching engine, `perf
record -g --call-graph dwarf` pointed at the benchmark harness itself: the
reusable `std::vector<Trade>` output buffer was never cleared between
events, so it grew unbounded across the whole 2M-event run, and its
periodic reallocations (each one relocating everything accumulated so far)
landed as multi-microsecond spikes inside whatever event's timing window
happened to trigger them — polluting the tail with noise that had nothing
to do with the matching engine.

Fix: `trades.clear()` after each event, **outside** the timed window (this
is harness bookkeeping, not engine work). That alone, before any change to
`OrderBook`:

| | before | after |
|---|---:|---:|
| mean | 180.7ns | 92.2ns |
| p99.9 | 1321.5ns | 628.0ns |
| p99.99 | 37,578ns | 10,227ns |
| max | 9,600,502ns | 326,307ns |

Worth stating plainly: the first thing profiling caught was the
measurement apparatus, not the code under test. That's not a footnote —
it's the difference between trusting a profiler's hotspot ranking and
checking that your own instrumentation isn't the thing being ranked.

### Step 2: profiling the corrected baseline

With that fixed, `perf record -g --call-graph dwarf` (software-event
sampling — this WSL2 VM doesn't expose hardware PMU counters to the guest;
`perf stat -e cycles,instructions` reports `<not supported>` here,
verified separately) on the corrected baseline showed
`OrderBook::cancelOrder` consuming **~8.3% of total CPU time**, almost
entirely as *self* time rather than time in a visible callee — meaning the
`std::map`'s `find`/`erase` calls were small enough to be inlined directly
into `cancelOrder`, so the RB-tree traversal and (on level teardown) node
deallocation were being paid for as if hand-written inline. The call graph
under `addOrder -> rest()` showed the same pattern on the insert side:
`std::map::operator[]` calling `lower_bound`, walking the tree to create a
new node for a new price level. Full excerpts:
[docs/profiling_notes.txt](docs/profiling_notes.txt).

This matched the predicted cost of the original representation
(`std::map<Price, PriceLevel>`, chosen deliberately as the
correctness-first baseline before any optimization work — see the history
note at the top of `order_book.hpp`): every price-level create/destroy is
a heap-allocated/deallocated RB-tree node, every lookup is O(log L)
pointer chasing.

### Step 3: the fix — `PriceLadder`

Replaced the per-side `std::map<Price, PriceLevel>` with `PriceLadder`: a
flat, pre-allocated array of `PriceLevel` indexed directly by price. A
level "exists" simply by being a slot in the array — creation and
teardown are free (no allocation, ever), and lookup is one array index
instead of a tree traversal. The trade-off, stated plainly: this
pre-allocates `maxPrice - minPrice + 1` slots whether or not they're used,
versus the tree's pay-only-for-what-you-touch allocation — the right call
for an instrument with a known, bounded tick range (true of any real
liquid instrument's practical trading range), not a general replacement
for a truly unbounded key space. A `Limit` order priced outside the
configured band is now rejected up front (same failure path as pool
exhaustion) — a real, explicit capability the tree had that the array
doesn't.

This was a **behavior-preserving internal change**: all 20 test cases
(569,265 assertions) from the correctness, concurrency, and
feed-simulator suites pass **unmodified** against the new representation
— same public `OrderBook` interface, different storage underneath.

### Step 4: did it work? — the honest answer

The instinct-level prediction was "cancel gets faster too, since the tree
traversal is gone." Measuring it directly said otherwise. Because a
single isolated benchmark run on this VM was not reproducible enough to
trust on its own (run-to-run variance of 25%+ was observed comparing two
separate isolated runs of *identical* binaries — consistent with
Hyper-V/WSL2 scheduling jitter, the same phenomenon visible in the
benchmark's own p99.99/max tail), the real comparison is an **interleaved
A/B**: baseline and optimized binaries built from the same commit-adjacent
sources, run back-to-back, 3 rounds, same benchmark harness
(full data: [docs/ab_comparison.txt](docs/ab_comparison.txt)):

| | baseline mean (ns) | optimized mean (ns) | change |
|---|---:|---:|---:|
| overall | 123.1 | 107.3 | **-12.8%** |
| Add | 105.0 | 81.7 | **-22.2%** |
| Cancel | 142.9 | 136.3 | -4.6% |
| Modify | 153.9 | 144.4 | -6.2% |

`Add` improved substantially and consistently across all 3 rounds — that's
the path that calls `rest()`, which is exactly where `std::map` was
allocating a new tree node per new price level. `Cancel` and `Modify`
improved only marginally, close to run-to-run noise.

Working hypothesis for the gap (**not confirmed with hardware counters** —
this environment doesn't expose cache-miss counts, so this is a
mechanistically-grounded inference from the access pattern, not a measured
fact): cancel's cost was likely never dominated by the container's
traversal *algorithm* so much as by a cache miss fetching an `Order`/
`PriceLevel` that had gone cold since it was last touched — replacing a
pointer-chasing tree lookup with an array-index computation removes
allocation and traversal overhead, but doesn't change whether the
resulting memory access is a cache hit or miss. `Add`'s improvement came
from removing real allocation cost on the level-creation path; `Cancel`
was likely memory-latency-bound on *both* implementations, so removing
the allocator didn't move it much. On bare-metal Linux with PMU access,
`perf stat -e cache-misses` would test this directly; this environment
couldn't.

This is the finding as measured, not a redraft toward the number that was
originally expected — and it's arguably more useful than a clean win
across the board would have been: it shows where the array actually pays
off (allocation-bound paths) and where a different fix would be needed
(memory-latency-bound paths — likely candidates: keeping the id index and
active-order pool from spreading order lookups across cold pages).

## Correctness testing

20 Catch2 test cases, 569,265 assertions, across four files:

- [tests/test_order_book.cpp](tests/test_order_book.cpp) — exact
  expected-outcome tests (not just aggregate invariants): time priority
  within a level, partial fills, multi-level market-order sweeps, unfilled
  limit remainder resting at its own price, cancel removing exactly the
  targeted order with others' priority intact, market-order edge cases
  (empty book, remainder beyond available liquidity dropped rather than
  rested), modify semantics (qty decrease keeps priority, price change
  loses it), duplicate-id and pool-exhaustion rejection, and a running
  conservation check (`poolInUse() == totalOrderCount()`) as a
  belt-and-suspenders no-lost/no-duplicated-orders net across a mixed
  sequence.
- [tests/test_spsc_queue.cpp](tests/test_spsc_queue.cpp) — the queue in
  isolation: FIFO order, full/empty behavior, a 100k-iteration wraparound
  stress test.
- [tests/test_concurrency.cpp](tests/test_concurrency.cpp) — real
  `std::thread`s, real queue, 200k events, checking the thread split
  changes nothing about correctness.
- [tests/test_feed_simulator.cpp](tests/test_feed_simulator.cpp) — event
  well-formedness, event-mix sanity bounds, and a 150k-event run through a
  real `OrderBook` checking the invariant holds and the aggressive-order
  fraction actually produces trades.

## Web demo

[web/server/server_main.cpp](web/server/server_main.cpp) is a small HTTP
server ([cpp-httplib](https://github.com/yhirose/cpp-httplib), vendored)
linking directly against `orderpulse_core` — every order the browser submits
goes through the exact same `OrderBook`/`PriceLadder` code as the tests and
benchmark, not a JavaScript reimplementation. [web/public/](web/public/)
is a static frontend (vanilla JS, no framework) showing a live bid/ask
ladder, an order entry form, resting-order cancellation, and a trade tape,
polling `GET /api/book` every 700ms.

This is a **deliberately different concurrency tradeoff** from the rest of
the project: `OrderBook` is single-threaded by design (see
[Concurrency design](#concurrency-design)), so the server serializes every
request that touches shared state behind one `std::mutex`. That's the
right call for a low-traffic demo serving occasional browser clicks — it
is not a claim about how to scale a real matching engine; the SPSC design
and the benchmark are where this project's actual latency-critical
engineering lives.

Run it locally:

```bash
cmake --build build -j --target orderpulse_server
./build/web/server/orderpulse_server        # listens on :8080 (or $PORT)
```

Then open `http://localhost:8080`. A "Simulate 200 events" button runs the
real `FeedSimulator` server-side to populate the book with realistic order
flow.

**Deployment**: `Dockerfile` and `render.yaml` build and run the server as
a [Render](https://render.com) web service (free tier). The Dockerfile is
a standard multi-stage build — compile in a full Ubuntu 24.04 toolchain
image, ship only the binary and static assets on top of `libstdc++6` in
the runtime stage (verified locally: the build steps, resulting binary's
dynamic library dependencies, and static-file serving from the exact
relative path layout Docker sets up were all checked directly against a
plain Ubuntu 24.04 environment, without requiring a running Docker daemon
for verification).

## Build & run

Requires CMake 3.16+ and a C++17 compiler. Developed and tested on Ubuntu
24.04 (WSL2) with GCC 13.3; no non-standard-library dependencies (Catch2
is vendored as a single header+source pair in `third_party/`, no network
access needed at build time).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/tests/orderpulse_tests      # correctness + concurrency + feed-simulator suite
./build/bench/orderpulse_bench      # latency benchmark, default 2,000,000 events
./build/bench/orderpulse_bench 500000 5000   # [num_events] [warmup_events]
```

### Reproducing the results

The before/after numbers in this README came from an interleaved A/B
comparison between two builds of the engine — the `std::map` baseline
(commit `86a69b6`) and the `PriceLadder` optimization (this repo's
current state) — using the *same* benchmark harness for both, run back to
back several times. `docs/ab_comparison.txt` documents the exact procedure
and raw output; `docs/results_baseline.txt` / `docs/results_optimized.txt`
are single representative runs of each, included for completeness. If
`perf` is available (Linux only):

```bash
perf record -F 4000 -g --call-graph dwarf -o perf.data -- ./build/bench/orderpulse_bench
perf report -i perf.data --stdio
```

## Honest limitations

- **Developed inside WSL2** (a Hyper-V VM), not bare metal. This VM does
  not expose hardware PMU counters to the guest (`perf stat -e
  cycles,instructions` reports `<not supported>`), so all profiling here
  is software-event (task-clock) sampling — real symbolized call-graph
  attribution, but no IPC/cache-miss/branch-misprediction data. Run-to-run
  variance on this host was substantial enough (25%+ between isolated
  runs of identical binaries) that every quantitative claim above is based
  on interleaved multi-round comparisons, not single runs.
- **`PriceLadder` requires a bounded price range**, configured at
  construction (default covers this project's tests and benchmark). A
  real deployment would size it to the instrument's actual realistic tick
  range; an order priced outside the configured band is rejected, not
  silently rerouted to a fallback structure.
- **The end-to-end latency metric reflects a deliberately saturated
  pipeline** (unthrottled producer), not a realistically-paced feed — see
  [Benchmark methodology](#benchmark-methodology).
- **The `Cancel`/`Modify` memory-latency hypothesis in
  [Profiling & optimization](#profiling--optimization) is inferred, not
  measured** — this environment couldn't get hardware cache-miss counts to
  confirm it directly.
"# OrderPulse" 
