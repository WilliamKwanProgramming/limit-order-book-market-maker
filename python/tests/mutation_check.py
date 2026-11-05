#!/usr/bin/env python3
"""Mutation check for the lobsim test suite: do the Python tests notice deliberate bugs?

Copies the sources the extension needs (root CMakeLists.txt, common/, core/, paper/, python/) into
a temporary directory, configures one build there, and then applies ONE deliberate bug
("mutant") at a time to the copy, rebuilds the lobsim target and runs pytest on python/tests.
A mutant is "caught" if at least one test fails. The working tree is never modified. Exits with
status 1 if a mutant survives or the unmutated baseline fails.

Most mutants are in the bindings (python/src/lobsim.cpp) or the stubs. Two are in the C++ engine
(core/), to check that the pure-Python differential test catches engine bugs by itself.

Usage: python3 python/tests/mutation_check.py [--out results/python/mutation_check.txt]
Needs cmake, ninja, a C++17 compiler, pybind11 >= 3.0, numpy and pytest.
"""

import argparse
import datetime
import os
import pathlib
import platform
import shlex
import shutil
import subprocess
import sys
import tempfile
import time

REPO = pathlib.Path(__file__).resolve().parents[2]
COPY = ["CMakeLists.txt", "common", "core", "paper", "python"]
LOBSIM = "python/src/lobsim.cpp"

# (name, file, text that must occur exactly once, replacement); several edits per mutant allowed.
MUTANTS = [
    ("P1 Side enum: Bid and Ask mapped to the opposite C++ values", [
        (LOBSIM, '.value("Bid", Side::Bid)\n      .value("Ask", Side::Ask)',
         '.value("Bid", Side::Ask)\n      .value("Ask", Side::Bid)')]),
    ("P2 Status enum: InvalidPrice and InvalidQty swapped", [
        (LOBSIM, '.value("InvalidPrice", Status::InvalidPrice',
         '.value("InvalidPrice", Status::InvalidQty'),
        (LOBSIM, '.value("InvalidQty", Status::InvalidQty',
         '.value("InvalidQty", Status::InvalidPrice')]),
    ("P3 depth: levels returned worst first", [
        (LOBSIM, "const Level& lv = levels_[static_cast<std::size_t>(i)];",
         "const Level& lv = levels_[static_cast<std::size_t>(k - 1 - i)];")]),
    ("P4 execute_resting: resting reports the executed size", [
        (LOBSIM, "out.r.resting = out.fills.back().resting_left;",
         "out.r.resting = out.fills.back().qty;")]),
    ("P5 replay: type 2 deletes the order instead of reducing it", [
        (LOBSIM, "case 2: ok = book.reduce(id(i), sz(i)) == Status::Ok; break;",
         "case 2: ok = book.cancel(id(i)) == Status::Ok; break;")]),
    ("P6 replay: top of book captured before the message instead of after", [
        (LOBSIM, "      if (capture_top) {\n        const auto lv_b",
         "      if (capture_top && i + 1 < n) {\n        const auto lv_b"),
        (LOBSIM, "        bp[k] = lv_b", "        bp[k + 1] = lv_b"),
        (LOBSIM, "        bq[k] = lv_b", "        bq[k + 1] = lv_b"),
        (LOBSIM, "        ap[k] = lv_a", "        ap[k + 1] = lv_a"),
        (LOBSIM, "        aq[k] = lv_a", "        aq[k + 1] = lv_a")]),
    ("P7 replay: an add with direction 0 is accepted as an ask", [
        (LOBSIM, "if (d == 1 || d == -1) {", "if (d == 1 || d == -1 || d == 0) {"),
        (LOBSIM, "static_cast<Side>(d)", "static_cast<Side>(d == 0 ? -1 : d)")]),
    ("P8 replay: rejected messages not counted per type", [
        (LOBSIM, "        ++rejects[slot];\n", "")]),
    ("P9 paper: 'symmetric' runs the time-matched rule", [
        (LOBSIM, "  lob::paper::ExperimentConfig cfg;",
         "  if (strat == lob::paper::Strategy::Symmetric) {\n"
         "    strat = lob::paper::Strategy::SymmetricTimeMatched;\n  }\n"
         "  lob::paper::ExperimentConfig cfg;")]),
    ("P10 stub: OrderBook.reduce parameter renamed", [
        ("python/lobsim/__init__.pyi", "def reduce(self, id: int, by_qty: int)",
         "def reduce(self, id: int, qty: int)")]),
    ("C1 core: replace keeps priority only when the size strictly shrinks", [
        ("core/src/order_book.cpp", "new_price == node.price && new_qty <= node.qty",
         "new_price == node.price && new_qty < node.qty")]),
    ("C2 core: execute_resting reports the wrong remaining size", [
        ("core/src/order_book.cpp",
         "Fill{id, kNoOrder, node.side, node.price, take, ts, node.qty - take}",
         "Fill{id, kNoOrder, node.side, node.price, take, ts, node.qty}")]),
]


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def pytest_run(work, build, timeout):
    env = dict(os.environ, PYTHONPATH=str(build / "python"), LOB_REPO_ROOT=str(REPO))
    t0 = time.monotonic()
    try:
        p = run([sys.executable, "-m", "pytest", str(work / "python" / "tests"), "-q", "-rf",
                 "--tb=no", "-p", "no:cacheprovider"], env=env, timeout=timeout)
    except subprocess.TimeoutExpired:
        return None, ["(timeout)"], time.monotonic() - t0, "timeout"
    failed = [ln.split(" - ")[0].replace("FAILED ", "").split("python/tests/")[-1]
              for ln in p.stdout.splitlines() if ln.startswith("FAILED")]
    summary = p.stdout.strip().splitlines()[-1] if p.stdout.strip() else p.stderr[-500:]
    return p.returncode, failed, time.monotonic() - t0, summary


def main():
    ap = argparse.ArgumentParser(description="Mutation check for the lobsim test suite.")
    ap.add_argument("--out", type=pathlib.Path, default=None)
    ap.add_argument("--timeout", type=float, default=1200.0, help="seconds per pytest run")
    ap.add_argument("--jobs", type=int, default=2)
    args = ap.parse_args()

    lines = []

    def log(s=""):
        print(s, flush=True)
        lines.append(s)

    commit = run(["git", "-C", str(REPO), "rev-parse", "HEAD"]).stdout.strip() or "unknown"
    dirty = run(["git", "-C", str(REPO), "status", "--porcelain", "--", "python", "core"]).stdout
    import numpy
    log("# Generated by python/tests/mutation_check.py; do not edit by hand.")
    log("# command: python3 " + " ".join(shlex.quote(a) for a in sys.argv))
    log(f"# git:     {commit} ({'uncommitted changes in python/ or core/' if dirty else 'clean'})")
    log(f"# date:    {datetime.datetime.now(datetime.timezone.utc):%Y-%m-%dT%H:%M:%SZ}")
    log(f"# python:  {platform.python_version()}; numpy {numpy.__version__}")
    log("")

    survived = 0
    with tempfile.TemporaryDirectory(prefix="lobsim_mutation_") as tmp:
        work = pathlib.Path(tmp) / "src"
        build = pathlib.Path(tmp) / "build"
        for item in COPY:
            src = REPO / item
            if src.is_dir():
                shutil.copytree(src, work / item, ignore=shutil.ignore_patterns(
                    "__pycache__", "*.pyc", ".pytest_cache"))
            else:
                (work / item).parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(src, work / item)
        cfg = run(["cmake", "-S", str(work), "-B", str(build), "-G", "Ninja",
                   "-DCMAKE_BUILD_TYPE=Release", "-DLOB_MODULES=core;paper;python",
                   "-DLOB_BUILD_TESTS=OFF", "-DLOB_BUILD_BENCH=OFF"])
        if cfg.returncode != 0 or "lobsim (python/): skipped" in cfg.stdout:
            log("configure failed or lobsim skipped:\n" + cfg.stdout[-2000:] + cfg.stderr[-2000:])
            return 1
        originals = {}

        def build_lobsim():
            return run(["cmake", "--build", str(build), "--target", "lobsim", "-j", str(args.jobs)])

        b = build_lobsim()
        if b.returncode != 0:
            log("baseline build failed:\n" + b.stdout[-3000:])
            return 1
        rc, failed, secs, summary = pytest_run(work, build, args.timeout)
        log(f"baseline (no mutant): {summary} [{secs:.0f} s]")
        if rc != 0:
            log("baseline must pass; aborting")
            return 1
        log("")

        for name, edits in MUTANTS:
            touched = {}
            try:
                for rel, old, new in edits:
                    path = work / rel
                    if rel not in originals:
                        originals[rel] = path.read_text()
                    text = touched.get(rel, originals[rel])
                    if text.count(old) != 1:
                        log(f"{name}: SETUP ERROR, pattern found {text.count(old)} times in {rel}")
                        return 1
                    touched[rel] = text.replace(old, new)
                for rel, text in touched.items():
                    (work / rel).write_text(text)
                b = build_lobsim()
                if b.returncode != 0:
                    log(f"{name}: caught at build time (does not compile)")
                    continue
                rc, failed, secs, summary = pytest_run(work, build, args.timeout)
                caught = rc != 0
                survived += not caught
                log(f"{name}: {'caught' if caught else 'SURVIVED'} - {summary} [{secs:.0f} s]")
                for f in failed[:4]:
                    log(f"    {f}")
                if len(failed) > 4:
                    log(f"    ... and {len(failed) - 4} more")
            finally:
                for rel in touched:
                    (work / rel).write_text(originals[rel])
        build_lobsim()

    log("")
    log(f"{len(MUTANTS) - survived} of {len(MUTANTS)} mutants caught.")
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text("\n".join(lines) + "\n")
        print(f"wrote {args.out}")
    return 1 if survived else 0


if __name__ == "__main__":
    sys.exit(main())
