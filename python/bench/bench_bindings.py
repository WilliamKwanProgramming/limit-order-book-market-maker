"""Benchmark of the lobsim bindings.

(a) Per-call overhead of the Python API: short loops of typical calls on a book holding about 300
    orders, reported as calls per second (loop overhead included, as in user code).
(b) Bulk replay throughput of lobsim.replay_lobster on a synthetic LOBSTER-style stream
    (python/bench/synthetic_stream.py), reported as messages per second.

Each measurement is repeated --runs times; the median, min and max are reported. Before starting,
the script waits (up to --wait seconds) until the 1-minute load average is below --max-load, and
marks the results as noisy if it never was. Output goes to stdout and to --out together with the
command line, git commit, Python, numpy and pybind11 versions, compiler and machine load.

    PYTHONPATH=build-python/python python3 python/bench/bench_bindings.py \
        --out results/python/bench.txt
"""

from __future__ import annotations

import argparse
import datetime
import os
import pathlib
import platform
import re
import shlex
import statistics
import subprocess
import sys
import time

import numpy as np

import lobsim
from lobsim import OrderBook, Side

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(HERE))
from synthetic_stream import COLUMNS, make_stream_with_messages  # noqa: E402


def load1() -> float:
    with open("/proc/loadavg") as f:
        return float(f.read().split()[0])


def loadavg() -> str:
    with open("/proc/loadavg") as f:
        return f.read().strip()


def wait_for_quiet(max_load: float, wait_s: float, log) -> bool:
    t0 = time.monotonic()
    while True:
        lv = load1()
        if lv < max_load:
            log(f"# 1-minute load {lv:.2f} < {max_load}: starting")
            return True
        if time.monotonic() - t0 > wait_s:
            log(f"# 1-minute load {lv:.2f} still >= {max_load} after {wait_s:.0f} s: running "
                "anyway; RESULTS ARE NOISY")
            return False
        time.sleep(10)


def git(*args: str) -> str:
    try:
        return subprocess.run(["git", "-C", str(REPO), *args], capture_output=True, text=True,
                              check=True).stdout.strip()
    except Exception:  # noqa: BLE001 - git missing or not a checkout
        return "unknown"


def cpu_model() -> str:
    try:
        for line in open("/proc/cpuinfo"):
            if line.startswith("model name"):
                return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor() or "unknown"


def summarize_rates(rates):
    return statistics.median(rates), min(rates), max(rates)


# ---- (a) per-call overhead -----------------------------------------------------------------------

def make_book(seed: int = 7) -> OrderBook:
    """About 300 resting orders over about 40 levels, away from the prices the loops use."""
    rng = np.random.default_rng(seed)
    book = OrderBook()
    book.reserve(4096, 256)
    oid = 1
    for _ in range(150):
        book.add_resting(oid, Side.Bid, int(990_000 - 100 * rng.integers(0, 20)), 100)
        oid += 1
        book.add_resting(oid, Side.Ask, int(1_010_000 + 100 * rng.integers(0, 20)), 100)
        oid += 1
    return book


class _Noop:
    def call(self, a, b, c, d, e):
        return None


def bench_calls(n: int):
    """Each entry: (name, calls per iteration, function running n iterations)."""
    base_id = 10_000_000

    def submit_cancel():
        book = make_book()
        sl, cancel, bid = book.submit_limit, book.cancel, Side.Bid
        t = time.perf_counter_ns()
        for i in range(n):
            sl(base_id + i, bid, 1_000_000, 100, i)
            cancel(base_id + i)
        return time.perf_counter_ns() - t

    def add_execute():
        book = make_book()
        add, ex, bid = book.add_resting, book.execute_resting, Side.Bid
        t = time.perf_counter_ns()
        for i in range(n):
            add(base_id + i, bid, 1_000_000, 100, i)
            ex(base_id + i, 100, i)
        return time.perf_counter_ns() - t

    def market_one_fill():
        book = make_book()
        add, mkt, bid, ask = book.add_resting, book.submit_market, Side.Bid, Side.Ask
        t = time.perf_counter_ns()
        for i in range(n):
            add(base_id + 2 * i, bid, 1_000_000, 100, i)
            r = mkt(base_id + 2 * i + 1, ask, 100, i)
            r.fills  # noqa: B018 - materialize the Fill objects
        return time.perf_counter_ns() - t

    def best_bid():
        book = make_book()
        f = book.best_bid
        t = time.perf_counter_ns()
        for _ in range(n):
            f()
        return time.perf_counter_ns() - t

    def depth10():
        book = make_book()
        f, bid = book.depth, Side.Bid
        t = time.perf_counter_ns()
        for _ in range(n):
            f(bid, 10)
        return time.perf_counter_ns() - t

    def length():
        book = make_book()
        t = time.perf_counter_ns()
        for _ in range(n):
            len(book)
        return time.perf_counter_ns() - t

    def py_noop():
        f = _Noop().call
        t = time.perf_counter_ns()
        for i in range(n):
            f(i, 1, 2, 3, 4)
        return time.perf_counter_ns() - t

    return [
        ("submit_limit (rests) + cancel", 2, submit_cancel),
        ("add_resting + execute_resting (1 fill)", 2, add_execute),
        ("add_resting + submit_market, read .fills", 2, market_one_fill),
        ("best_bid()", 1, best_bid),
        ("depth(Side.Bid, 10) -> (10, 3) array", 1, depth10),
        ("len(book)", 1, length),
        ("pure-Python no-op method, 5 args (scale)", 1, py_noop),
    ]


# ---- (b) bulk replay -----------------------------------------------------------------------------

def bench_replay(stream, runs: int, capture_top: bool):
    cols = [stream[c] for c in COLUMNS]
    n = len(cols[0])
    book = OrderBook()
    lobsim.replay_lobster(book, *cols, capture_top=capture_top)  # warm-up (allocation, caches)
    times = []
    for _ in range(runs):
        book.clear()
        t = time.perf_counter_ns()
        res = lobsim.replay_lobster(book, *cols, capture_top=capture_top)
        times.append(time.perf_counter_ns() - t)
    rates = [n / (ns * 1e-9) for ns in times]
    return rates, times, res, book


def core_replay_median(path: pathlib.Path):
    """Median msgs/second of the replay stream in results/core/bench.txt, or None."""
    try:
        text = path.read_text()
    except OSError:
        return None
    m = re.search(r"replay timing.*?\n\s*median\s+[\d.]+\s+([\d.e+]+)", text, re.S)
    return float(m.group(1)) if m else None


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--messages", type=int, default=5_000_000)
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--runs", type=int, default=7)
    ap.add_argument("--calls", type=int, default=200_000, help="iterations per call loop")
    ap.add_argument("--max-load", type=float, default=1.3)
    ap.add_argument("--wait", type=float, default=600.0, help="max seconds to wait for low load")
    ap.add_argument("--out", type=pathlib.Path, default=None)
    args = ap.parse_args(argv)

    lines: list[str] = []

    def log(s=""):
        print(s, flush=True)
        lines.append(s)

    dirty = git("status", "--porcelain", "--", "python")
    info = lobsim._lobsim.__build_info__
    cmd = " ".join(shlex.quote(a) for a in ["python3", *sys.argv])
    log("# Generated by python/bench/bench_bindings.py; do not edit by hand.")
    log(f"# command:  PYTHONPATH={os.environ.get('PYTHONPATH', '')} {cmd}")
    log(f"# git:      {git('rev-parse', 'HEAD')} "
        f"({'uncommitted changes in python/' if dirty else 'clean python/'})")
    log(f"# date:     {datetime.datetime.now(datetime.timezone.utc):%Y-%m-%dT%H:%M:%SZ}")
    log(f"# python:   {platform.python_version()}; numpy {np.__version__}; "
        f"pybind11 {info['pybind11']}")
    so_path = pathlib.Path(lobsim._lobsim.__file__).resolve()
    # Show the extension relative to the repository so that no machine-specific path is recorded.
    log(f"# lobsim:   {so_path.relative_to(REPO) if so_path.is_relative_to(REPO) else so_path.name}")
    log(f"# compiler: {info['compiler']}; build type {info['build_type']}; flags "
        f"'{info['cxx_flags']}' (plus pybind11_add_module's -fvisibility=hidden and LTO)")
    log(f"# cpu:      {cpu_model()}, {os.cpu_count()} logical CPUs")
    log("# machine:  shared 2-core cloud VM; other jobs may run concurrently, so the numbers are")
    log("#           indicative only and will differ on other hardware.")
    quiet = wait_for_quiet(args.max_load, args.wait, log)
    log(f"# loadavg at start: {loadavg()}")
    log()

    log(f"(a) per-call overhead: {args.calls} iterations per loop, {args.runs} runs, "
        "book of 300 orders")
    log(f"  {'call':44s} {'median calls/s':>15s} {'min':>11s} {'max':>11s} {'median ns/call':>15s}")
    for name, per_iter, fn in bench_calls(args.calls):
        fn()  # warm-up
        rates = []
        for _ in range(args.runs):
            ns = fn()
            rates.append(args.calls * per_iter / (ns * 1e-9))
        med, lo, hi = summarize_rates(rates)
        log(f"  {name:44s} {med:15.3e} {lo:11.3e} {hi:11.3e} {1e9 / med:15.1f}")
    log(f"# loadavg after (a): {loadavg()}")
    log()

    t0 = time.perf_counter()
    stream = make_stream_with_messages(args.messages, args.seed)
    gen_s = time.perf_counter() - t0
    n = len(stream["type"])
    log(f"(b) bulk replay: synthetic stream, {n} messages, seed {args.seed} "
        f"(generated in {gen_s:.1f} s, not timed), {args.runs} runs after one warm-up run")
    counts = np.bincount(stream["type"].astype(np.int64), minlength=8)
    names = {1: "add_resting", 2: "reduce (partial cancel)", 3: "cancel (deletion)",
             4: "execute_resting"}
    for t, name in names.items():
        log(f"  type {t} {name:26s} {counts[t]:9d} {100 * counts[t] / n:6.1f}%")
    medians = {}
    for capture in (False, True):
        rates, times, res, book = bench_replay(stream, args.runs, capture)
        med, lo, hi = summarize_rates(rates)
        medians[capture] = med
        label = "with capture_top=True" if capture else "capture_top=False"
        log(f"  {label}: rejected {res.rejected}; book at end: {len(book)} orders, "
            f"{book.level_count(Side.Bid)} bid and {book.level_count(Side.Ask)} ask levels")
        log("    per-run ns/msg: " + " ".join(f"{t / n:.1f}" for t in times))
        log(f"    msgs/s median {med:.3e}  min {lo:.3e}  max {hi:.3e}  "
            f"(median {1e9 / med:.1f} ns/msg)")
        if capture:
            both = (res.bid_price > 0) & (res.ask_price > 0)
            crossed = both & (res.bid_price >= res.ask_price)
            log(f"    top of book locked or crossed after {100 * crossed.mean():.1f}% of "
                "messages")
    log(f"# loadavg at end: {loadavg()}")
    log()

    core_file = REPO / "results" / "core" / "bench.txt"
    core = core_replay_median(core_file)
    if core is None:
        log("C++ comparison: results/core/bench.txt not found or not parsed; no comparison.")
    else:
        ratio = medians[False] / core
        same = int(np.floor(np.log10(medians[False]))) == int(np.floor(np.log10(core)))
        log("C++ comparison: median bulk-replay msgs/s (capture_top=False) divided by the median "
            "replay msgs/s")
        core_rev = git("log", "-1", "--format=%h", "--", str(core_file))
        log(f"  in results/core/bench.txt (git {core_rev}"
            f"): {ratio:.2f}; {'same' if same else 'different'} order of magnitude. The two "
            "streams are")
        log("  generated differently (message mix and book shape differ), so only the order of "
            "magnitude is comparable.")
    if not quiet:
        log("NOTE: load was above --max-load at the start; treat these numbers as noisy.")

    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text("\n".join(lines) + "\n")
        print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
