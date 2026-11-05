# lobster: LOBSTER replay and per-message book validation

`lob::lobster` stream-parses LOBSTER message and order-book files, replays the messages into
`lob::OrderBook` (replay mode, see `core/README.md`), and after every message compares the
engine's top $N$ levels with the matching row of the order-book file. `lobster_replay` is the
command-line front end.

**What has been validated, and what has not.** There is no real LOBSTER file in this repository
or in the environment it was developed in. Everything here was developed and tested against
synthetic files in the LOBSTER layout written by `tools/synth_lobster.py`, an independent
standard-library Python generator with its own book (a dict of price levels, linear scans; it
shares no code or design with `core/`). Agreement between the two on millions of messages is a
cross-implementation check of the parser, the replay mapping and the engine. It is not evidence
about real data: both sides were written for this repository from the same reading of the LOBSTER
documentation, so a misreading shared by both would not show up. **Behaviour on real LOBSTER files
is untested.** The tool is written to fail loudly (strict policy, exact line numbers) rather than
guess when real data differs from what is assumed below.

## File format as implemented

Summarised from the public LOBSTER documentation in `docs/design.md` ("LOBSTER format notes").

| File | Row (no header) | Parsed as |
|---|---|---|
| message | `time,type,order_id,size,price,direction` | time to integer nanoseconds; the rest `int64` |
| order book | `ask_px_1,ask_sz_1,bid_px_1,bid_sz_1, ...` for levels $1..N$ | $4N$ `int64` fields |

- **Time** is seconds after midnight, `S` or `S.F` with 1 to 9 fractional digits, converted to
  nanoseconds by integer arithmetic (`34200.1` is $34200 \cdot 10^9 + 10^8$ ns). Rejected: more
  than 9 fractional digits, a leading `-` (negative time), an empty fraction (`34200.`), signs,
  exponents and anything else that is not digits.
- **Price** is an integer in LOBSTER units (USD $\times 10^4$) and is used unchanged as
  `lob::Price`. **Direction** $+1$ is a buy limit order (`Side::Bid`), $-1$ a sell limit order
  (`Side::Ask`); for executions it is the side of the resting order.
- **Per-type rules.** Types 1-4: order id $> 0$, size $> 0$, price $> 0$, direction $\pm 1$.
  Types 5 and 6: size $\ge 0$; id, price and direction unrestricted. Type 7 (trading halt): no
  field is restricted. Other types are rejected. The buy/sell and positive-price rules are never
  applied to types 5, 6 and 7.
- **Order-book rows.** $N$ is inferred from the first row (or given with `--levels`, which must
  then match). Each level is either (price $> 0$, size $> 0$) or the side's empty-level sentinel
  (ask `9999999999`, bid `-9999999999`) with size 0; anything else is a malformed row.
- **Robustness.** CRLF or LF line ends, a last line without a newline, spaces and tabs around
  fields, one trailing comma, a UTF-8 byte-order mark at the start, and blank lines (skipped and
  counted) are accepted. A malformed row is reported with its line number, field number and reason.
  Parsing never throws and never allocates; integers use `std::from_chars`, time is parsed by hand.
- **Streaming.** Each file is read through one 1 MiB buffer (it grows only for a single line
  longer than that), so memory does not depend on the file size.

**Open questions about the real format** (none can be settled without a real file; the parser is
lenient where these matter and the replayer counts what it sees):

- How many fractional digits real files use, and whether trailing zeros are dropped. Any count from
  0 to 9 is accepted.
- Which values type 5, 6 and 7 rows carry in the id, size, price and direction fields (for
  example whether hidden executions have id 0). They are accepted as written and never touch the
  book.
- Whether order-book rows for type 5, 6 and 7 messages always repeat the previous row. The
  validator checks this and counts violations separately.
- Whether a type 2 row can cancel the whole remaining size, or a type 3 row can carry a size that
  differs from the remaining size. Both are counted as anomalies, not assumed impossible.
- Whether a real sample starts with an empty book. If the window starts after orders were
  submitted, messages refer to ids that never appeared as type 1 (see the bootstrap below).
- Whether order ids can be reused within a day. The engine forgets ids once an order leaves the
  book, so reuse after removal replays correctly; a type 1 for an id still resting is counted as a
  duplicate.

**What public pages show** (web pages and course material read on 2026-11-05, not a file processed
here; sources in `docs/design.md`): a real window can start with a deletion or an execution of an
order that the file never submits, so the strict run is expected to stop at message 1 and
`--policy lenient --bootstrap` is the likely route (step 3 below), which has been tested only on
the synthetic cut files; message rows end in a trailing comma (accepted); the opening and closing
auction crosses are type 6 with order ids -1 and -2 (accepted, but the synthetic generator writes
id 0); halt rows carry direction -1 (accepted, the generator writes 0); and the direction of
type 5 rows is called not observable, so the buy/sell split of hidden executions in `--stats`
should not be read as a side.

## Replay and anomaly policy

| Message | Engine call |
|---|---|
| 1 new limit order | `add_resting(id, side, price, size, time)` |
| 2 partial cancellation | `reduce(id, size)` |
| 3 deletion | `cancel(id)` |
| 4 visible execution | `execute_resting(id, size, time)` |
| 5 hidden execution, 6 cross trade, 7 halt | none (counted) |

Every type 2, 3 and 4 message is checked against the resting order it names. Anomaly counters,
one per kind: `unknown_id` (type 2/3/4 id not resting), `duplicate_id` (type 1 id already
resting), `size_exceeds_remaining` (type 2/4 size larger than what remains),
`partial_cancel_removes_order` (type 2 size equal to what remains), `side_mismatch`,
`price_mismatch`, `delete_size_mismatch` (type 3 size not equal to what remains; these three are
the "field mismatches"), and `time_backwards` (time earlier than the previous message's).

- `--policy strict` (default): the first anomaly or malformed row stops the run. The message is
  not applied; its line number, fields and anomaly kinds are printed.
- `--policy lenient`: count and continue. The engine call is skipped where the engine would reject
  it (unknown or duplicate id); otherwise it is made, and the engine caps an oversized reduce or
  execution at the remaining size. Malformed rows are counted and their row pair is not validated.

## Bootstrap heuristic (`--bootstrap`, off by default)

**A heuristic, untested on real data; the CLI says so whenever it is used.** A message file that
starts after the open can refer to orders submitted earlier. `--bootstrap` takes the first
order-book row, undoes the first message on it to get the book just before that message, and
rests one synthetic "ghost" order per visible level (ids with the top bit set, so they cannot
collide with LOBSTER ids), holding the level's whole size. A later type 2, 3 or 4 message whose id
is unknown is applied to the ghost order(s) at the message's side and price, oldest first: types 2
and 3 reduce the ghost by the message size, type 4 executes it. The run reports ghosts created and
emptied, references resolved, references that stay unexplained (no ghost at that side and price;
also counted as `unknown_id`) and shortfalls (the ghost holds less than the message size; also
counted as `size_exceeds_remaining`).

Limits. Only the $N$ visible levels can be seeded: orders that were deeper than level $N$ at the
start stay unknown, and their levels are missing from the engine when the price moves to them. A
ghost stands for all earlier orders at its price together, so it has one queue position (ahead of
every order added in the window, which is correct for orders that existed before the window). If
the first message cannot be undone consistently (for example a type 1 larger than the level it
landed on), the run reports `first_message_undone=no` and seeds from the row as it stands.

## Validation and statistics

After each message the engine's top $N$ levels per side (`OrderBook::depth`) are compared with
the row; a level the engine does not have must be the sentinel. Reported: rows checked, rows with
any mismatch, split into price-structure mismatches (some price or emptiness differs) and size-only
mismatches, mismatch counts per level and side, the first mismatch (message number, line, fields,
expected and actual levels) and the last mismatching message. Rows for types 5, 6 and 7 must also
equal the previous row (counted separately).

`--stats` adds: counts per type; executed volume per side (type 4 by resting side; type 5 by
direction); the time-weighted mean spread, where the book after message $k$ is weighted by
$t_{k+1} - t_k$ and only time with both sides non-empty counts; the time-weighted mean size at the
best bid and at the best ask; and the fraction of messages after which a best price (or a best
price or its size) differs from before.

## Command line

```
lobster_replay --messages FILE [--book FILE] [--levels N] [--policy strict|lenient]
               [--bootstrap] [--stats] [--bench] [--bench-passes K] [--out DIR] [--quiet]
```

A plain-text summary goes to stdout; with `--out DIR` a `key=value` summary is written to
`DIR/summary.txt` (and `DIR/bench.txt` with `--bench`). Exit code: 0 if nothing is wrong; 1 if,
under the strict policy, there is an anomaly or a malformed row, or (with `--book`) any row
mismatches or a type 5/6/7 row differs from the previous row; 2 for usage and I/O errors, an empty
message file, or files with different row counts. Under the lenient policy anomalies and malformed
rows are reported but do not by themselves give exit code 1. `--bench` times, over the same files,
parse-only (message file), parse+replay, and parse+replay+validate (both files) passes and reports
the best, median and worst throughput with the load average.

## Running on a real LOBSTER sample

Nothing is downloaded by this repository. LOBSTER offers free sample files on its website,
lobsterdata.com. To try one:

1. Download a sample zip by hand into `data/raw/` (git-ignored; never commit market data) and
   unzip it there. Each sample has a message file and an order-book file whose names end in
   `_message_N.csv` and `_orderbook_N.csv`, where $N$ is the number of levels.
2. Build (see below) and run, writing outputs under `data/raw/` as well:

   ```sh
   build-lobster/lobster/lobster_replay \
     --messages data/raw/<name>_message_<N>.csv --book data/raw/<name>_orderbook_<N>.csv \
     --levels <N> --stats --out data/raw/replay_<N>
   ```
3. If the strict run stops at an `unknown_id` near the start, the window probably starts with
   orders already in the book. Rerun with `--policy lenient --bootstrap` and read the ghost and
   mismatch counters. If it stops for another reason, the printed line and fields show which
   assumption above the file breaks.

## Synthetic data and tests

`tools/synth_lobster.py --messages M --levels N --seed S --out PREFIX [--cut K]` writes
`PREFIX_message_N.csv` and `PREFIX_orderbook_N.csv`. Its toy flow (documented in the script's
docstring; not a model of any real market) draws one event per step: new limit orders at
exponentially distributed distances behind the best price, sometimes one tick better or strictly
inside the spread; deletions; partial cancellations; executions of the front order at the touch;
marketable orders that sweep several resting orders as several type 4 rows with one timestamp
(sometimes leaving a type 1 remainder at the limit price, with the same timestamp); rare sweeps
that empty a whole side; hidden executions (type 5), cross trades (type 6) and halts (type 7,
price $-1$, 0 or 1, direction 0) that leave the book unchanged. Ids are unique and increasing with
gaps; timestamps are non-decreasing with about 10% repeated; 30% of times are written without
trailing zeros. The script asserts that its book is never crossed or locked. `--cut K` simulates
$K$ messages first and writes only what follows, so the files start with a non-empty book.

Tests (CTest, `ctest -R lobster`):

- `lobster_test_format`: parser and reader edge cases (9-digit fractions, `34200`, `34200.1`,
  CRLF, trailing commas, BOM, malformed rows of every kind, sentinels, level-count checks), plus
  randomized comparisons against deliberately naive `std::string` reference parsers for time
  (200,000 strings) and integers (200,000), a message round trip (50,000 rows), and the line
  reader with random line lengths and buffer sizes down to 16 bytes.
- `lobster_test_replay`: replay semantics of every message type against hand-computed books,
  every anomaly counter, strict versus lenient, the validator (a corrupted size flagged at exactly
  that message; price-structure versus size-only; empty side versus sentinel; type 5/6/7 rows),
  file alignment errors, the bootstrap on a hand-built window, and `--stats` on hand-computed
  examples.
- `lobster_integration` (Python 3; skipped with code 77 if CMake finds no interpreter): four
  generated files of 200,000 messages (seeds 1-4, $N$ = 1, 5, 10, 10) must replay under the strict
  policy with zero anomalies, zero malformed rows and zero mismatches on every row; adding 1 to one
  size in one row must give exactly one mismatching row, at that message; and on a cut file the
  strict run must stop at an unknown id while `--bootstrap` must at least halve the lenient
  mismatch rate (the measured rates are printed in the test log).
- `lobster_test_alloc`: a full session (parse, replay, validate, stats) over 12,000 and over
  120,000 messages must make the same number of heap allocations (counted by replacing global
  `operator new`), so nothing is allocated per message once the buffers exist.
- `python3 lobster/tests/mutation_check.py --out results/lobster/mutation_check.txt` applies 12
  deliberate bugs one at a time to a temporary copy of the sources (time scaling, CR handling, the
  line reader's refill, the side of type 1, type 2 as a deletion, an anomaly check, two bootstrap
  bugs, two validator bugs, stats weighting, a per-message allocation) and requires at least one
  test program to fail for each. All 12 are caught (`results/lobster/mutation_check.txt`).

## Results

Produced by `lobster/reproduce.sh` (commands, seeds, commit and machine load in
`results/lobster/MANIFEST.txt`; per-run `summary.txt` files next to it). All files are synthetic.

<!-- tables:begin -->

**Table 1.** Strict replay of synthetic files with `--book` (every row checked).

| Seed | Messages | Levels N | Exit code | Malformed rows | Anomalies | Rows checked | Rows with a mismatch | Type 5/6/7 rows equal to previous |
|---|---|---|---|---|---|---|---|---|
| 101 | 500000 | 1 | 0 | 0 | 0 | 500000 | 0 | 16784 of 16784 |
| 101 | 500000 | 5 | 0 | 0 | 0 | 500000 | 0 | 16784 of 16784 |
| 101 | 500000 | 10 | 0 | 0 | 0 | 500000 | 0 | 16784 of 16784 |
| 102 | 500000 | 1 | 0 | 0 | 0 | 500000 | 0 | 16942 of 16942 |
| 102 | 500000 | 5 | 0 | 0 | 0 | 500000 | 0 | 16942 of 16942 |
| 102 | 500000 | 10 | 0 | 0 | 0 | 500000 | 0 | 16942 of 16942 |

**Table 2.** What the synthetic files contain (from the generator's own counters). For one seed the message stream does not depend on N; only the order-book file does.

| Seed | Levels N | Type 1 / 2 / 3 / 4 / 5 / 6 / 7 | Multi-row sweeps | Rows with an empty side | Rows with fewer than N levels | Rows sharing a timestamp |
|---|---|---|---|---|---|---|
| 101 | 1 | 216495 / 23800 / 108422 / 134499 / 11887 / 2952 / 1945 | 12661 | 4025 | 4025 | 133097 |
| 101 | 5 | 216495 / 23800 / 108422 / 134499 / 11887 / 2952 / 1945 | 12661 | 4025 | 38562 | 133097 |
| 101 | 10 | 216495 / 23800 / 108422 / 134499 / 11887 / 2952 / 1945 | 12661 | 4025 | 203066 | 133097 |
| 102 | 1 | 216279 / 23663 / 108586 / 134530 / 11851 / 3061 / 2030 | 12893 | 3887 | 3887 | 132754 |
| 102 | 5 | 216279 / 23663 / 108586 / 134530 / 11851 / 3061 / 2030 | 12893 | 3887 | 36700 | 132754 |
| 102 | 10 | 216279 / 23663 / 108586 / 134530 / 11851 / 3061 / 2030 | 12893 | 3887 | 201823 | 132754 |

**Table 3.** `--stats` on the same files (toy flow; descriptive only). Spread in LOBSTER price units (1 = $0.0001; the toy grid is 100 units).

| Seed | Levels N | Mean spread (price units) | Mean best bid size | Mean best ask size | Messages changing a best price | Executed vs bids / asks |
|---|---|---|---|---|---|---|
| 101 | 1 | 139.1 | 690.8 | 693.9 | 7.7430% | 9997622 / 10279016 |
| 101 | 5 | 139.1 | 690.8 | 693.9 | 7.7430% | 9997622 / 10279016 |
| 101 | 10 | 139.1 | 690.8 | 693.9 | 7.7430% | 9997622 / 10279016 |
| 102 | 1 | 136.9 | 686.4 | 689.9 | 7.6342% | 10257673 / 10233255 |
| 102 | 5 | 136.9 | 686.4 | 689.9 | 7.6342% | 10257673 / 10233255 |
| 102 | 10 | 136.9 | 686.4 | 689.9 | 7.6342% | 10257673 / 10233255 |

**Table 4.** Window that starts mid-day: seed 201, 500000 messages written after 100000 simulated and dropped (--cut).

| Levels N | Run | Messages processed | Unknown-id anomalies | Rows with a mismatch | Mismatch rate | Last mismatching message | Ghosts created / references resolved / unexplained / shortfalls |
|---|---|---|---|---|---|---|---|
| 1 | strict | stopped at message 1 | 1 | 0 | n/a | - | - |
| 1 | lenient | 500000 | 72 | 147 | 0.0294% | 713 | - |
| 1 | lenient + bootstrap | 500000 | 66 | 131 | 0.0262% | 713 | 2 / 6 / 66 / 0 |
| 5 | strict | stopped at message 1 | 1 | 0 | n/a | - | - |
| 5 | lenient | 500000 | 72 | 561 | 0.1122% | 713 | - |
| 5 | lenient + bootstrap | 500000 | 32 | 500 | 0.1000% | 713 | 10 / 40 / 32 / 0 |
| 10 | strict | stopped at message 1 | 1 | 0 | n/a | - | - |
| 10 | lenient | 500000 | 72 | 713 | 0.1426% | 713 | - |
| 10 | lenient + bootstrap | 500000 | 9 | 453 | 0.0906% | 713 | 20 / 63 / 9 / 0 |

**Table 5.** Throughput on a 2,000,000-message synthetic file, N = 10, 7 passes per stage (median of the passes; best and worst also shown). Build: GNU 13.3.0, Release, -O3 -DNDEBUG. Load average 1.00 before and 1.08 after (shared 2-core VM).

| Stage | Messages | Best msgs/s | Median msgs/s | Worst msgs/s |
|---|---|---|---|---|
| parse | 2000000 | 7.26e+06 | 6.61e+06 | 5.25e+06 |
| parse+replay | 2000000 | 4.32e+06 | 3.72e+06 | 3.1e+06 |
| parse+replay+validate | 2000000 | 8.74e+05 | 8.05e+05 | 6.88e+05 |

<!-- tables:end -->

**Reading.**

- Tables 1 and 2: on 3,000,000 checked rows (two seeds of 500,000 messages, each written with
  three level counts) the engine's book
  equals the Python book after every message, with no anomaly, and every type 5/6/7 row repeats
  the previous row. The files contain multi-row sweeps, empty sides, rows with fewer than $N$
  levels and repeated timestamps. This is agreement between two implementations written for this
  repository, not a check against real data.
- Table 4: a file that starts with a non-empty book stops at message 1 under the strict policy, as
  intended. In this toy flow the last mismatching row is message 713 in every run (the book turns
  over quickly), so even without the bootstrap only 0.03% to 0.14% of rows mismatch. The
  bootstrap removes some of these (713 to 453 rows at $N = 10$) but not most. The remaining
  mismatches come from levels that were deeper than $N$ at the start and later move into the top
  $N$ (in the per-level counts of `summary.txt` they concentrate at the deepest levels). The
  smaller $N$ is, the less the bootstrap can see: at $N = 1$ it resolves 6 references and leaves
  66 unexplained. On real data, where orders may rest much longer, both the damage and what the
  bootstrap can repair are unknown.
- Table 5: on this shared VM, with the load average near 1.0, the full pass (parse both files,
  replay, compare 10 levels per side) ran at a median of 0.81 million messages per second (best
  0.87, worst 0.69), and parse plus replay of the message file alone at a median of 3.7 million
  (best 4.3, worst 3.1). The spread between passes is large because the VM is shared, so read
  these as roughly one million and four million messages per second, not as precise figures. The
  order-book file is 5.8 times larger than the message file (465 MB against 81 MB, sizes in
  `MANIFEST.txt`); how the difference splits between parsing it and comparing levels was not
  measured. The core benchmark (`core/README.md`) is not comparable: it replays from memory
  without parsing.

## Build, test, reproduce

```sh
cmake -S . -B build-lobster -G Ninja -DLOB_WERROR=ON -DLOB_MODULES="core;lobster"
cmake --build build-lobster -j2
ctest --test-dir build-lobster -R lobster --output-on-failure
lobster/reproduce.sh build-lobster [data-dir]       # rewrites results/lobster/ (about 5 min)
python3 lobster/tests/mutation_check.py --out results/lobster/mutation_check.txt
```

## Limitations

- Untested on real LOBSTER data (see the top of this file). The format details listed as open
  questions are assumptions.
- The bootstrap is a heuristic with the limits stated above; with it, mismatches can remain until
  the orders it cannot see have left the book.
- Validation covers only the $N$ levels in the order-book file; deeper levels are replayed but not
  checked.
- The synthetic flow is a toy: its book turns over within about a thousand messages and holds at
  most about 130 orders, so the cut experiment and the throughput figures say nothing about how
  the bootstrap or the replay would fare on a real trading day.
- Types 5, 6 and 7 are counted, never interpreted (no hidden-liquidity or auction modelling).
- The replayer uses `lob::OrderBook` unchanged; any engine limitation (see `core/README.md`)
  applies.
