# core: limit order book engine

`lob::OrderBook` (`include/lob/order_book.hpp`) is a single-threaded C++17 limit order book with
price-time priority. It has two sets of calls on one data structure: **matching mode** for the
simulator (`submit_limit`, `submit_market`, `cancel`, `reduce`, `replace`) and **replay mode** for
LOBSTER-style data, where nothing matches and executions arrive as messages (`add_resting`,
`execute_resting`, `cancel`, `reduce`). Errors come back as `lob::Status` codes; fills are appended
to a caller-owned `std::vector<Fill>`. The header documents every function, edge case and
complexity.

## Design

| Piece | Structure | Why |
|---|---|---|
| Orders | pooled nodes in a `std::vector` with a free list, 32-bit indices | no allocation per order once warm; indices survive pool growth |
| Queue per price | intrusive doubly linked FIFO through the nodes | O(1) append, O(1) removal from anywhere |
| Levels per side | `std::vector` sorted with the **best price at the back** | best = `back()`, emptying it = `pop_back()`, inserts near the touch move only a few levels |
| Id lookup | open-addressing hash (linear probing, backward-shift delete, load <= 1/2) | O(1) expected, no tombstones after heavy cancelling |

| Call | Cost (L levels on that side, d = levels between price and touch, k fills) |
|---|---|
| `best_bid/ask`, `mid`, `spread`, `front`, counts, totals | O(1) |
| `find` | O(1) expected |
| `add_resting`, resting part of `submit_limit` | O(min(d, 8) + log L) lookup + O(d) if a level is created |
| `cancel`, `reduce`, `execute_resting`, `replace` keeping priority | O(1) id lookup + the same level lookup |
| matching part of `submit_limit`/`submit_market` | O(k + levels emptied) |
| `depth(side, n)` | O(n) |
| `queue_ahead(id)` | O(orders ahead of `id` in its level), **not O(1)** |

## Build, test, benchmark

```bash
cmake -S . -B build-core -G Ninja -DLOB_WERROR=ON && cmake --build build-core -j2
ctest --test-dir build-core -R core --output-on-failure
core/bench/run_bench.sh --compare-levels    # writes results/core/bench*.txt
```

Tests: `test_order_book` (30 hand-checked unit tests) and two randomized differential tests that
replay 2.4 M matching-mode and 2.2 M replay-mode operations (4 seeds each) against a deliberately
naive `std::map<Price, std::deque<...>>` model (`tests/reference_book.hpp`), comparing status codes,
every fill and full depth after every operation, and calling `check_invariants()` every 1000
operations. As a sanity check that the tests can fail, two bugs were injected by hand (replace kept
priority on a size increase; a partial reduce skipped the side total); both were caught.

## Results snapshot

From `results/core/bench.txt` (GCC 13.3, `-O3 -DNDEBUG`, Release, 20 M synthetic messages, 7 runs,
seed 42, load average 0.52 at start). **Measured on a shared 2-core cloud VM** (Intel Xeon
@ 2.80GHz) while another job could run; treat as indicative.

| Stream | best ns/msg | median ns/msg | worst ns/msg | median msgs/s |
|---|---|---|---|---|
| replay (48.9% add, 41.1% delete, 0.8% partial cancel, 9.2% execute) | 59.9 | 63.5 | 65.6 | 1.57e7 |
| matching (49.3% passive limit, 35.5% cancel, 7.6% marketable limit, 7.6% market) | 62.4 | 66.9 | 70.6 | 1.49e7 |

The books hold about 300 orders over 41 to 51 price levels. The streams are **synthetic**
(generated in `bench/bench_replay.cpp`), not market data.

**Level container comparison** (`results/core/bench_map_levels.txt`, same benchmark with
`-DLOB_CORE_MAP_LEVELS=ON`, load 0.69 at start): with `std::map` levels the median was 61.7 ns/msg
(replay) and 81.4 ns/msg (matching). Replay is a tie within run-to-run noise; matching is about
18% faster with the sorted vector, so the vector is the default. Both variants pass all tests and
give identical fill checksums. A direct-indexed price ladder was **not** tried.

## Limitations

No self-trade prevention, no order types beyond limit and market (no IOC/FOK/hidden/iceberg), no
thread safety. `queue_ahead` walks the queue. `reduce` and `execute_resting` cap an oversized
quantity at the remaining size instead of rejecting it.
