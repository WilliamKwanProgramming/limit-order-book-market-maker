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
| `backtest/` | Event-driven market simulator, quoting strategies, Hawkes informed-flow stress test | `lob::backtest`, `stress_test` |
| `python/`   | pybind11 module `lobsim` and pytest tests | `lobsim` |
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
commit. Nothing is typed in by hand.

**Data.** No market data is stored in this repository, and none may be committed. Tests use
synthetic data. Real LOBSTER sample files are downloaded separately and kept under `data/raw/`,
which is git-ignored.

## Core order book contract (`core/`)

The engine is single-threaded. It supports two modes on the same data structures.

**Matching mode** (used by the simulator), price-time priority:

- `submit_limit(id, side, price, qty, ts)`: any part that crosses the opposite best price trades
  immediately, best price first and oldest first within a level, at the resting order's price. The
  remainder rests at `price`.
- `submit_market(id, side, qty, ts)`: sweeps the opposite side up to `qty`. The unfilled part is
  discarded.
- `cancel(id)`: removes an order entirely.
- `reduce(id, by_qty)`: partial cancel that keeps queue position; removes the order if nothing
  remains.
- `replace(id, new_price, new_qty)`: keeps priority only if the price is unchanged and the size
  does not increase; otherwise the order goes to the back of the queue. It may cross and then
  matches.
- Fills are reported with resting id, aggressor id, resting side, price (the resting order's), size
  and time, without allocating per fill in steady state.

**Replay mode** (used for LOBSTER data), where executions arrive as separate messages:

- `add_resting(id, side, price, qty, ts)`: adds an order without matching. The book is allowed to
  be locked or crossed in this mode.
- `execute_resting(id, qty)`: reduces a resting order as if executed against an external
  aggressor, and reports a fill with `aggressor_id = kNoOrder`.
- `cancel` and `reduce` as above.

**Queries**, constant time unless noted: best bid and best ask (aggregated size and order count),
mid price, spread, top-`n` aggregated levels on each side (`depth`), lookup of an order by id
(side, price, remaining size, timestamp), `queue_ahead(id)` (shares ahead of the order in its price
level), number of orders, number of price levels, and total size per side.

**Checks.** `check_invariants()` (linear time, for tests): per-level size equals the sum of its
orders, the id index matches the levels, price levels are sorted and non-empty, and in matching
mode the best bid is below the best ask.

Errors (unknown id, duplicate id, non-positive size or price) are reported through return codes,
never exceptions. The header documents the complexity and edge cases of every function.

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
