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
operations. To check that the tests can fail, `python3 core/tests/mutation_check.py` applies eight
deliberate bugs one at a time to a temporary copy of the sources (a wrong matching comparison, the
replace priority rule, hash-table deletion, level search, a fill field, a side total, queue order)
and requires at least one test program to fail for each. All eight are caught
(`results/core/mutation_check.txt`).

## Results snapshot

From `results/core/bench.txt` (run from the commit recorded in that file; GCC 13.3, `-O3
-DNDEBUG`, Release, 20 M synthetic messages, 7 runs, seed 42). **Measured on a shared 2-core cloud
VM** (Intel Xeon @ 2.80 GHz) whose load average was 0.00 at the start and 0.34 at the end of the
run; treat the numbers as indicative.

| Stream | best ns/msg | median ns/msg | worst ns/msg | median msgs/s |
|---|---|---|---|---|
| replay (48.9% add, 41.1% delete, 0.8% partial cancel, 9.2% execute) | 58.2 | 62.0 | 69.6 | 1.61e7 |
| matching (49.3% passive limit, 35.5% cancel, 7.6% marketable limit, 7.6% market) | 65.6 | 77.2 | 88.7 | 1.30e7 |

The speed of this VM varies between sessions. `results/core/bench_busy_vm_session.txt` is an earlier
run of the same code on a busier VM, with medians of 75.4 ns/msg (replay) and 89.9 ns/msg
(matching), which are 22% and 16% slower than the medians above. Read the results as "roughly
11 to 16 million messages per second on this VM", not as a precise figure. The books hold about 300
orders over 41 to 51 price levels, so the working set is small and fits in cache. The streams are
**synthetic** (generated in `bench/bench_replay.cpp`), not market data.

**Level container comparison** (`results/core/bench_compare_levels.txt`). The default sorted
vector (A) and the `std::map` variant (B, `-DLOB_CORE_MAP_LEVELS=ON`) were run on the same streams
in 8 counterbalanced AB/BA blocks, so that drift in the VM's speed cannot be mistaken for an effect
of the container. The vector was faster in 7 of 8 blocks on the replay stream (median ratio B/A
1.14) and in 8 of 8 blocks on the matching stream (median ratio 1.20). Both variants pass all tests
and produce identical fill checksums. A direct-indexed price ladder was **not** tried.

## Limitations

No self-trade prevention, no order types beyond limit and market (no IOC/FOK/hidden/iceberg), no
thread safety. `queue_ahead` walks the queue. `reduce` and `execute_resting` cap an oversized
quantity at the remaining size instead of rejecting it.
