# Design notes

Shared conventions and module contracts for this repository. Update this file whenever an
interface changes.

## Layout

| Directory   | Contents | CMake targets |
|-------------|----------|---------------|
| `common/`   | Header-only basics: `types.hpp`, `rng.hpp`, `check.hpp` (test harness) | `lob::common` |
| `core/`     | Order book engine with price-time priority, unit and randomized tests, benchmark | `lob::core` |
| `paper/`    | Reproduction of the simulation in Avellaneda and Stoikov (2008) | `paper_replication` |
| `lobster/`  | LOBSTER message and order-book file parser, replayer, per-message book validator, synthetic data generator, independent Python reference book | `lob::lobster`, `lobster_replay` |
| `backtest/` | Event-driven market simulator, quoting strategies, Hawkes informed-flow stress test | `lob::backtest`, `backtest_run`, `stress_test` |
| `python/`   | pybind11 extension `lobsim._lobsim` and the pure-Python package `lobsim` (type stubs, pytest tests, call-overhead and bulk-replay benchmark) | `lobsim` |
| `results/`  | Generated tables and figures, one subdirectory per module | none |
| `docs/`     | This file | none |

Each module owns its directory, including its `CMakeLists.txt`, and does not edit another
module's files. The root `CMakeLists.txt` adds a module automatically when
`<module>/CMakeLists.txt` exists, so no root edits are needed.

## Conventions

**Build.** Out-of-source:
`cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build --output-on-failure`.
Options: `LOB_SANITIZE=ON` (ASan and UBSan), `LOB_WERROR=ON`, `LOB_NATIVE=ON` (local experiments
only; published numbers use the default flags).
`LOB_MODULES` (default: all) selects the modules to configure, for example
`-DLOB_MODULES="core;paper;lobster"`; list every module a target depends on.

**Language.** C++17 without compiler extensions. Warning-free with GCC 13 and Clang 18 under
`-Wall -Wextra -Wpedantic -Wshadow -Wconversion`, and clean under `LOB_SANITIZE=ON`. Prefer
portable code (no `__int128`, no `__builtin_*` outside a small guarded helper) so the project also
builds with MSVC. No exceptions on hot paths. No heap allocation per message in steady state.

**Style.** `snake_case` functions and variables, `CamelCase` types, `kCamelCase` constants,
trailing underscore on private data members, `#pragma once`, 100 columns, namespace `lob`. Public
headers in `<module>/include/lob/`, sources in `<module>/src/`, tests in `<module>/tests/`.

**Units.** `Price` is an integer number of ticks. Replay code uses the LOBSTER convention of
1 tick = 0.0001 USD. Simulation code states its tick size explicitly. `Qty` is shares. `Time` is
nanoseconds. `Side::Bid = +1` and `Side::Ask = -1` match LOBSTER's direction field.

**Determinism.** All randomness goes through `lob::Rng`. No `std::random_device`, no wall-clock
seeding, no `<random>` distributions anywhere that produces a reported number. Every experiment
takes an explicit seed.

**Tests.** CTest, with the dependency-free harness in `common/include/lob/check.hpp`. Python
tests use pytest. Prefer randomized differential tests against a slow but obviously correct model
over hand-picked examples alone.

**Results.** Every number quoted in a README or in docs comes from a command in this repository
that writes its output under `results/<module>/` together with the command line, seed and git
commit. Nothing is typed in by hand. Tables in module READMEs sit between `tables:begin` and
`tables:end` markers and are rewritten by the module's `make_tables.py`; prose that quotes a
number from a table must be re-checked whenever the results are regenerated. The reproduce
scripts use paths relative to the repository root, so no machine-specific path ends up in a
committed file, and each `MANIFEST.txt` records the commit and whether the module's working tree
was clean. Results are always regenerated from a committed tree.

**Data.** No market data is stored in this repository, and none may be committed. Tests use
synthetic data. Real LOBSTER sample files are downloaded separately and kept under `data/raw/`,
which is git-ignored. Scratch files that the reproduce scripts write (synthetic LOBSTER-layout
files, deleted after use) go under `data/tmp/`, also git-ignored.

**Benchmarks.** Timings are only meaningful on a quiet machine. The reproduce scripts wait for the
one-minute load average to drop below 1.3 and record the load before and after; numbers from the
shared 2-core VM are indicative, not hardware claims.

## Core order book contract (`core/`)

`core/include/lob/order_book.hpp` is the authoritative description (every function, edge case and
complexity is documented there); this section is the summary other modules rely on. The engine is
single-threaded and supports two modes on the same data structures.

**Matching mode** (used by the simulator), price-time priority. Each call that can trade appends
`Fill` records to a caller-owned `std::vector<Fill>` (never cleared by the book) and returns a
`SubmitResult{status, filled, resting}`:

- `submit_limit(id, side, price, qty, ts, fills)`: any part that crosses the opposite best price
  trades immediately, best price first and oldest first within a level, at the resting order's
  price. The remainder rests at `price`.
- `submit_market(id, side, qty, ts, fills)`: sweeps the opposite side up to `qty`. The unfilled
  part is discarded.
- `replace(id, new_price, new_qty, ts, fills)`: `new_qty` is the new remaining size. Priority is
  kept only if the price is unchanged and the size does not increase; otherwise the order goes to
  the back of the queue with timestamp `ts`, and it may cross and then match.
- `cancel(id)` removes an order. `reduce(id, by_qty)` is a partial cancel that keeps queue
  position and removes the order if `by_qty >= remaining`. Both return a `Status`.
- A `Fill` carries resting id, aggressor id, resting side, price (the resting order's), size, the
  time of the aggressing call and the resting order's remaining size after the fill.

**Replay mode** (used for LOBSTER data), where executions arrive as separate messages:

- `add_resting(id, side, price, qty, ts)`: adds an order without matching. The book may become
  locked or crossed.
- `execute_resting(id, qty, ts, fills)`: executes `min(qty, remaining)` of a resting order as if
  against an external aggressor and appends one fill with `aggressor_id = kNoOrder`.
- `cancel` and `reduce` as above. The two modes may be mixed on one book.

**Queries.** Constant time unless noted: `best(side)`, `best_bid`, `best_ask` (price, aggregated
size, order count), `mid` (ticks, as double), `spread`, `front(side)` (oldest order at the best
price), `find(id)` (side, price, remaining size, queue timestamp), `order_count`, `level_count`,
`total_qty`. `depth(side, n, out)` is O(n). `queue_ahead(id)` is O(position in its level), not
constant. `check_invariants(require_uncrossed, why)` is linear and meant for tests: pass
`require_uncrossed = false` for replay mode.

**Errors** are reported through `Status` (`Ok`, `InvalidId`, `InvalidPrice`, `InvalidQty`,
`DuplicateId`, `UnknownId`) and never thrown; a rejected call leaves the book unchanged. Id 0 is
reserved (`kNoOrder`). Ids are not remembered once an order leaves the book, so they may be reused.
An id that is already resting is rejected as `DuplicateId`, also for market orders. `reduce` and
`execute_resting` cap an oversized quantity at what remains instead of rejecting it. There is no
self-trade prevention (orders carry no owner).

## LOBSTER format notes (`lobster/`)

Summarised from the public LOBSTER documentation; confirm against a real sample file when one is
available.

*Message file*, no header, one row per event: time (seconds after midnight, fractional up to
nanoseconds), type, order id, size, price (dollars times 10000), direction. Types: 1 new limit
order; 2 partial cancellation (size is the amount cancelled); 3 deletion of the whole order;
4 execution of a visible limit order; 5 execution of a hidden limit order (does not change the
visible book); 6 cross trade (auction; does not change the visible book); 7 trading halt
indicator. Direction is -1 for a sell limit order and 1 for a buy limit order; for an execution it
is the side of the resting order that was hit.

*Order-book file*, no header, one row per message giving the book state **after** that message,
with `N` levels as `ask price, ask size, bid price, bid size` repeated for levels 1 to `N`. Empty
levels use ask price 9999999999, bid price -9999999999 and size 0.

*Points to treat with care.* None of these has been checked against a real file in this repository.

- A single incoming order that sweeps several resting orders appears as several type-4 messages
  with the same timestamp, so one message is not one trade.
- Trading-halt rows (type 7) use unusual field values: id 0, size 0, price -1 (halt), 0 (quoting)
  or 1 (resume), and direction -1 according to LOBSTER's documentation as quoted in its demo code;
  the order-book row repeats the previous one. Parse them without applying the buy/sell and
  positive-price rules that hold for the other types, and never let them change the book. Type 5
  and type 6 rows likewise leave the visible book unchanged.
- A message file that starts after the open can mention orders that were submitted earlier and so
  never appear as a type-1 row, even though the order-book file already contains them. A replayer
  has to decide what to do with such ids; it must count them and say how it treated them. Public
  examples show this: the first row of a SAP window read in a university course is a type-3
  deletion of an order that the file never submits (see the sources below).
- The order-book file shows only the top `N` levels, but the messages affect every level.
- Public readers of the message file name a seventh column `null` and drop it, presumably because
  rows end in a trailing comma (no source shows the raw bytes). The parser accepts one trailing
  empty field in message and order-book rows and rejects a non-empty seventh field.
- Cross trades in the opening and closing auctions are type-6 rows with order id -1 (opening) and
  -2 (closing) according to the same course. The parser accepts any id for type 6. The synthetic
  generator writes id 0 for cross trades and direction 0 for halts, which differs from the real
  files.
- For executions against hidden orders (type 5) the course calls the direction "not observable"
  and builds a proxy from prices, so `--stats` reports type-5 volume by "direction as written in
  the file; not observable per the public course notes", not as a buy/sell side. Rows in the form
  these sources show (halt rows, crosses with id -1 and -2, hidden executions, trailing commas) are
  test fixtures in `lobster/tests/test_format.cpp` and `test_replay.cpp`.

*Public sources read for these notes (pages, not files; no LOBSTER file has been processed):*
the comments of LOBSTER's demo code as quoted at
https://www.r-bloggers.com/2019/12/converting-lobster-demo-r-code-into-python (halt rows,
sentinels, row layout) and the exercise at
https://aef-course.ku.dk/exercises/high-frequency-econometrics.html (seventh column, auction ids,
hidden-execution direction, first rows of a SAP window); the reader at
https://voigtstefan.github.io/post/lobster-1 (seventh column); and the dataset viewer at
https://huggingface.co/datasets/y7pan/lobster-data, which shows rows of LOBSTER's AAPL 2012-06-21
sample, among them hidden executions with id 0 and direction -1 or +1. The first page's comment
lists the buy direction as -2, which contradicts the sample rows and the second page ("direction =
1 if executed against a limit buy order"); this repository uses +1 for buy.
