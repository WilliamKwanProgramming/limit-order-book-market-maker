# python: lobsim bindings

`lobsim` is a Python package around a pybind11 extension (`lobsim._lobsim`) that exposes the
order book engine of `core/` (`lob::OrderBook`) and the Avellaneda-Stoikov (2008) replication of
`paper/` (`lob::paper::run_experiment`). It adds one function of its own, `replay_lobster`, which
applies numpy arrays of LOBSTER-style messages to a book in a C++ loop without the GIL. The
engine semantics are those of `core/include/lob/order_book.hpp`; nothing is reimplemented in
Python.

## Build and test

Verified in this repository with CMake 3.28, Ninja, GCC 13.3 and Clang 18.1, Python 3.13,
numpy 2.x and pybind11 3.1:

```bash
pip install pybind11 numpy pytest          # pybind11 >= 3.0 is required
cmake -S . -B build-python -G Ninja -DLOB_WERROR=ON -DLOB_MODULES="core;paper;python"
cmake --build build-python -j2
ctest --test-dir build-python --output-on-failure        # includes the test python_pytest
PYTHONPATH=build-python/python python3 -m pytest python/tests -q   # the same tests directly
```

The importable package is assembled in `build-python/python/lobsim/` (extension, `__init__.py`,
`paper.py`, the `.pyi` stubs and `py.typed`), so `PYTHONPATH=build-python/python` is all that is
needed to `import lobsim`.

The module is optional. `python/CMakeLists.txt` skips it with a status message, and the C++
targets and tests are unaffected, when the Python interpreter or development headers are not
found (`find_package(Python3 COMPONENTS Interpreter Development.Module)`), when pybind11 >= 3.0
is not found (`find_package(pybind11 CONFIG)`, hinted with `python3 -m pybind11 --cmakedir`), when
`core` or `paper` is not in `LOB_MODULES`, or when `LOB_SANITIZE=ON` (the interpreter is not
built with ASan). The CTest entry `python_pytest` runs `tests/run_pytest.py`, which reports the test
as skipped (exit code 77) if pytest or numpy is not installed or the extension was not built.

**pip route.** `python/pyproject.toml` builds a wheel with scikit-build-core from the repository
root (only the `core`, `paper` and `python` modules, no tests):

```bash
pip install ./python        # from the repository root; needs network access for build deps
```

This was verified in a fresh virtual environment (wheel built, installed, full pytest suite
passed against the installed package). It works only from a full checkout, because the CMake
project is the repository root; no sdist is provided.

## API

| Name | What it is |
|---|---|
| `Side` | `enum.IntEnum`: `Bid = 1`, `Ask = -1` (LOBSTER's direction values) |
| `Status` | `enum.Enum`: `Ok`, `InvalidId`, `InvalidPrice`, `InvalidQty`, `DuplicateId`, `UnknownId` |
| `Fill`, `Level`, `OrderInfo` | read-only value classes with the C++ fields, `==`, `hash` and a readable `repr` |
| `SubmitResult` | `status`, `filled`, `resting`, `fills` (tuple of `Fill` from this call only), `ok` |
| `OrderBook()` | `reserve(max_orders, max_levels)`, `clear()` |
| matching mode | `submit_limit(id, side, price, qty, ts=0)`, `submit_market(id, side, qty, ts=0)`, `replace(id, new_price, new_qty, ts=0)` -> `SubmitResult` |
| both modes | `cancel(id)`, `reduce(id, by_qty)` -> `Status` |
| replay mode | `add_resting(id, side, price, qty, ts=0)` -> `Status`; `execute_resting(id, qty, ts=0)` -> `SubmitResult` |
| queries | `best(side)`, `best_bid()`, `best_ask()` -> `Level` or `None`; `mid()` -> `float` or `None`; `spread()`, `queue_ahead(id)` -> `int` or `None`; `front(side)`, `find(id)` -> `OrderInfo` or `None`; `depth(side, n)`; `order_count()`, `len(book)`, `level_count(side)`, `total_qty(side)`; `check_invariants(require_uncrossed=True)` -> `(ok, message)` |
| `replay_lobster(book, time_ns, type, order_id, size, price, direction, *, capture_top=False)` | bulk replay without the GIL -> `ReplayResult` |
| `paper.simulate(gamma, sims, seed, strategy="inventory", *, s0=100, sigma=2, horizon=1, A=140, k=1.5, steps=200)` | -> `paper.SimResults` (read-only numpy arrays `profit`, `final_q`, `mean_spread`, `cash`, `final_mid`, `bid_fills`, `ask_fills`) |
| `paper.summarize(results, first=0, count=None)` | `lob::paper::summarize` as a dict with the columns of `results/paper/*/summary.csv` |

Every function and class has a docstring; `lobsim/__init__.pyi` and `lobsim/paper.pyi` are the type
stubs.

Design choices:

- **Results and fills together.** C++ appends fills to a caller-owned vector. In Python each call
  that can trade returns one `SubmitResult` whose `fills` holds the fills of that call only. The
  `Fill` objects are created when `fills` is read, so a call whose fills are not read pays only for
  the C++ vector. `execute_resting` returns `Status` in C++; here it returns a `SubmitResult` with
  `filled` = executed size, `resting` = size left under that id and the single fill, so that
  "status and fills" come back together as in the matching calls.
- **Errors.** As in C++, a rejected call returns a `Status` (or a `SubmitResult` with that status)
  and leaves the book unchanged; nothing is raised for bad values. Arguments of the wrong Python
  type raise `TypeError` (pybind11 overload resolution): plain `int` for a `Side`, floats for ids,
  prices or sizes, negative ids, or values that do not fit in `int64`/`uint64`. numpy integer
  scalars are accepted. `depth(side, n)` with `n < 0` raises `ValueError`.
- **`depth(side, n)`** returns a new numpy `int64` array of shape `(k, 3)`, columns
  `[price, qty, count]`, best price first, `k = min(n, level_count(side))`.
- **`replay_lobster`** takes six 1-D numpy arrays of equal length with exact dtypes: `time_ns`
  int64, `type` int8, `order_id` uint64, `size` int64, `price` int64 (ticks), `direction` int8.
  Strided views are accepted. Message types follow `docs/design.md` in replay mode: 1
  `add_resting(order_id, Side(direction), price, size, time_ns)` (a direction other than +1/-1 is
  rejected), 2 `reduce(order_id, size)`, 3 `cancel(order_id)`, 4
  `execute_resting(order_id, size, time_ns)`, 5 to 7 no change; any other type value is rejected.
  A message is rejected when the engine returns a status other than `Ok`; the book is then
  unchanged. `ReplayResult` has `messages`, `rejected`, `first_rejected` (-1 if none), and int64
  arrays `count_by_type` and `rejected_by_type` of length 8 (index t for type t = 1..7, index 0 for
  any other type value). With `capture_top=True` it also has `bid_price`, `bid_qty`, `ask_price`,
  `ask_qty` after each message, with price 0 and size 0 for an empty side. A non-array argument
  raises `TypeError`; a wrong dtype (including non-native byte order), a non-1-D array or
  mismatched lengths raise `ValueError` naming the column. It is a thin loop over the engine and
  does not depend on the `lobster/` module (which has its own parser, replayer and validator).
- **GIL.** `replay_lobster` and `paper.simulate` release the GIL. While `replay_lobster` runs on a
  book, any call on that book from another thread raises `RuntimeError` instead of racing.
- **`paper.simulate`** calls `lob::paper::run_experiment` with one gamma and one strategy.
  Simulation `i` uses `Rng::for_stream(seed, i)`, as the `paper_replication` tool does, so the same
  seed gives the same numbers as `results/paper/`. Invalid model parameters raise `ValueError`.

## Example

```python
import lobsim
from lobsim import OrderBook, Side

book = OrderBook()
book.submit_limit(1, Side.Ask, 10_001, 300)
book.submit_limit(2, Side.Ask, 10_002, 500)
book.submit_limit(3, Side.Bid, 9_999, 200)
r = book.submit_limit(4, Side.Bid, 10_002, 400, ts=5)   # marketable
print(r.status, r.filled, r.resting, r.fills)           # Status.Ok 400 0 (Fill(...), Fill(...))
print(book.depth(Side.Ask, 5))                          # [[10002   400     1]]
print(book.queue_ahead(3), book.spread())               # 0 3

res = lobsim.paper.simulate(gamma=0.1, sims=1000, seed=1, strategy="inventory")
print(lobsim.paper.summarize(res))   # matches the gamma=0.1 inventory row of
                                     # results/paper/sims1000/summary.csv
```

`examples/quickstart.py` runs a longer version and writes a figure (book depth and the profit
histograms of the inventory and symmetric strategies) to the path given on the command line:

```bash
PYTHONPATH=build-python/python python3 python/examples/quickstart.py /tmp/quickstart.png
```

## Tests

`tests/` (pytest; run as above):

- `test_book_behavior.py`: the scenarios of `core/tests/test_order_book.cpp` through the Python
  API (price-time priority, partial fills, multi-level sweeps, market orders, cancel and reduce,
  the replace priority rules, every error status, `queue_ahead`, depth shape and order, crossed
  books in replay mode, `reserve`/`clear`, more levels than the linear-scan window, extreme ids
  and prices).
- `test_differential.py`: randomized differential test against `reference_book.py`, an
  independent pure-Python book (dict of price -> deque per side) written from the contract, not
  from the C++ source. Seeds 1, 2, 3 in matching mode and in replay mode, 100,000 operations each
  (`LOBSIM_DIFF_OPS` changes this), including invalid ones (id 0, unknown and duplicate ids,
  non-positive prices and sizes). After every operation it compares status, sizes and every
  fill, the top of book, counts and totals, the best 5 levels per side, `front`, and `find` and
  `queue_ahead` of a random id; every 2,000 operations it compares the full depth, every resting
  order and `check_invariants()`. Replay mode mixes in a few matching calls and is locked or
  crossed for a large share of the steps.
- `test_replay.py`: `replay_lobster` equals the same stream applied call by call through the
  scalar API (synthetic streams of about 60,000 to 90,000 messages, with 0%, 5% and 20% extra
  invalid or ignored messages): top of book after every message, rejection counts per type, first
  rejected index, final depth and every order. Also chunked replay, strided arrays, crossed
  books, empty arrays, the dtype, length, dimension and type errors, and a threaded test showing
  that the GIL is released and that the book refuses calls during a replay.
- `test_paper.py`: `paper.simulate` with the seed read from `results/paper/sims1000/run.txt`
  reproduces every row of `results/paper/sims1000/summary.csv` (relative tolerance 1e-9; the file
  has 10 significant digits) and every per-simulation value in `sims.csv`; plus prefix stability,
  common random numbers across strategies, model keywords, `summarize` against numpy, and
  parameter errors.
- `test_api.py`: enum values and types, value classes (read-only, equality, hash, repr),
  `TypeError` for wrong argument types, numpy scalars, keyword arguments.
- `test_stubs.py`: parameter names of every stubbed function equal the signature pybind11 records
  in the runtime docstring, every public callable is stubbed, and `mypy.stubtest` (allowlist in
  `tests/stubtest_allowlist.txt`) passes; the stubtest part is skipped when mypy is not installed
  (the CI job installs mypy 2.4.1, so it runs there).

**Mutation check.** `tests/mutation_check.py` copies the sources to a temporary directory, applies
one deliberate bug at a time there (the working tree is not touched), rebuilds the extension and
runs the suite. The mutants are wrong enum mappings, reversed depth, wrong `execute_resting`
fields, wrong replay semantics, top-of-book capture off by one, a wrong strategy mapping, a renamed
stub parameter, and two bugs in the C++ engine (replace priority, execute fill size) that only the
Python differential and behavior tests can see from here. Result in
`results/python/mutation_check.txt`: all 12 mutants were caught, each by at least one failing
test; the unmutated baseline passed.

```bash
python3 python/tests/mutation_check.py --out results/python/mutation_check.txt
```

## Performance

`bench/bench_bindings.py` writes `results/python/bench.txt` (command, git commit, Python, numpy,
pybind11 and compiler versions, load average):

```bash
PYTHONPATH=build-python/python python3 python/bench/bench_bindings.py --out results/python/bench.txt
```

From `results/python/bench.txt` (git 471cdd1, GCC 13.3 Release, CPython 3.13.18, numpy 2.5.3,
7 runs each, load average 1.08 at start). **Measured on a shared 2-core cloud VM** (Intel Xeon
@ 2.80GHz) while other jobs could run; treat as indicative.

Per-call overhead, book of 300 orders, loop overhead included (200,000 iterations per loop):

| Call | median calls/s | median ns/call |
|---|---|---|
| `submit_limit` (rests) + `cancel` | 1.40e6 | 714.1 |
| `add_resting` + `execute_resting` (1 fill) | 1.42e6 | 704.5 |
| `add_resting` + `submit_market`, reading `.fills` | 9.07e5 | 1102.1 |
| `best_bid()` | 3.29e6 | 304.4 |
| `depth(Side.Bid, 10)` | 1.28e6 | 779.7 |
| `len(book)` | 6.33e6 | 157.9 |
| pure-Python no-op method with 5 arguments, for scale | 2.05e7 | 48.8 |

One bound call costs a few hundred nanoseconds. Most of that is binding overhead: `len(book)`,
which does almost no work in C++, takes 157.9 ns.

Bulk replay of a 5,000,000-message synthetic stream (seed 42; 45.5% adds, 4.5% partial cancels,
34.1% deletions, 15.9% executions; about 300 orders on 25 to 32 levels per side):

| `replay_lobster` | median msgs/s | min | max | median ns/msg |
|---|---|---|---|---|
| `capture_top=False` | 1.47e7 | 1.24e7 | 1.56e7 | 68.1 |
| `capture_top=True` | 1.42e7 | 1.12e7 | 1.45e7 | 70.5 |

This is the same order of magnitude as the C++ replay benchmark in `results/core/bench.txt`
(the script prints the ratio of medians). The two streams are generated differently, so only the
order of magnitude is comparable. A Python loop making one scalar call per message would pay the
per-call costs in the first table on every message, against 68.1 ns per message for
`replay_lobster` (that loop was not benchmarked).

## Limitations

- The book is single-threaded, as in C++. The bindings refuse concurrent calls only while
  `replay_lobster` runs; two Python threads calling ordinary methods are serialized by the GIL.
  On a free-threaded Python build the module would re-enable the GIL (it does not declare
  free-threading support).
- No pickle or copy support for `OrderBook`, `SubmitResult`, `ReplayResult` or `SimResults`.
- Each scalar call costs on the order of a few hundred nanoseconds of binding overhead (see
  Performance); loops of millions of calls belong in `replay_lobster` or in C++.
- `replay_lobster` covers replay mode only (types 1 to 7); there is no bulk matching-mode call and
  no bulk capture of fills. It does not parse LOBSTER files or convert prices; that is the
  `lobster/` module's job.
- Only the matching and replay calls, queries and the A&S `simulate`/`summarize` are bound. The
  paired bootstrap, quantiles and trace output of `paper/` are not.
- Building the extension sets `POSITION_INDEPENDENT_CODE` on the `lob_core` and `lob_paper` static
  libraries in that build directory (needed to link them into a shared object). This changes
  their code generation in build trees that include `python/`; the published core benchmark uses
  its own build directory.
- The synthetic streams for the tests and the benchmark come from numpy's `default_rng` with
  explicit seeds, not from `lob::Rng`; they are test inputs, not reported experiment results.
- Verified only on Linux x86-64 (this VM and the Ubuntu 24.04 CI runners) with CPython 3.12 (CI
  only) and 3.13. Other platforms and versions are untested.
