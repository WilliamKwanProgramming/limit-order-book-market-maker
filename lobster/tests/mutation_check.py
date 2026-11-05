#!/usr/bin/env python3
"""Mutation check for the lobster test suite: do the tests notice deliberate bugs?

Copies the sources the lobster tests need into a temporary directory, applies ONE deliberate bug
("mutant") at a time, rebuilds lobster_replay, lobster_cut_full_seed and the three unit-test
programs, and runs the unit tests and the integration test (tests/integration_test.py, about 25 s
per mutant). A mutant is "caught" if at least one of the four fails. A surviving mutant means a
blind spot; the script then exits with status 1. Pattern: core/tests/mutation_check.py.

Usage: python3 lobster/tests/mutation_check.py [--out FILE] [--timeout SECONDS]
Standard library only; needs cmake, a C++17 compiler and (optionally) ninja.
"""
import argparse
import datetime
import pathlib
import shutil
import subprocess
import sys
import tempfile
import time

UNIT = ["lobster_test_format", "lobster_test_replay", "lobster_test_alloc"]
TARGETS = UNIT + ["lobster_replay", "lobster_cut_full_seed"]

# (name, file, text that must occur exactly once, replacement)
MUTANTS = [
    ("L1 time: fraction scaled by 10^8 instead of 10^9", "lobster/src/format.cpp",
     "for (std::size_t k = digits; k < 9; ++k) frac *= 10;",
     "for (std::size_t k = digits; k < 8; ++k) frac *= 10;"),
    ("L2 reader: CR before LF not stripped", "lobster/src/reader.cpp",
     "      if (len > 0 && buf_[begin_ + len - 1] == '\\r') --len;\n"
     "      line = std::string_view(buf_.data() + begin_, len);\n      begin_ = pos + 1;",
     "      line = std::string_view(buf_.data() + begin_, len);\n      begin_ = pos + 1;"),
    ("L3 reader: search after a refill skips one byte", "lobster/src/reader.cpp",
     "scan = begin_ + scanned;", "scan = begin_ + scanned + 1;"),
    ("L4 replay: type 1 rests on the opposite side", "lobster/src/replayer.cpp",
     "case 1: s = book_.add_resting(id, m.side(), m.price",
     "case 1: s = book_.add_resting(id, opposite(m.side()), m.price"),
    ("L5 replay: type 2 handled as a full deletion", "lobster/src/replayer.cpp",
     "case 2: s = book_.reduce(id, m.size); break;", "case 2: s = book_.cancel(id); break;"),
    ("L6 anomaly: partial cancel of the whole order not flagged", "lobster/src/replayer.cpp",
     "} else if (m.size == info->qty) {", "} else if (m.size == info->qty + 1) {"),
    ("L7 bootstrap: a deletion removes the whole ghost order", "lobster/src/replayer.cpp",
     "        s = book_.reduce(g.id, take);", "        s = book_.cancel(g.id);"),
    ("L8 bootstrap: first message not undone before seeding", "lobster/src/replayer.cpp",
     "  if (changes_visible_book(first.type)) {\n    ghosts_.first_message_undone",
     "  if (false) {\n    ghosts_.first_message_undone"),
    ("L9 validator: deepest level not compared", "lobster/src/validator.cpp",
     "  for (std::size_t i = 0; i < n; ++i) {\n    const BookLevel& ea",
     "  for (std::size_t i = 0; i + 1 < n; ++i) {\n    const BookLevel& ea"),
    ("L10 validator: type 5/6/7 rows not compared with the previous row",
     "lobster/src/validator.cpp", "    if (!(row == prev_)) {", "    if (false) {"),
    ("L11 stats: interval weighted by the state after the message", "lobster/src/stats.cpp",
     "  if (have_time_ && m.time > last_time_) {",
     "  top_ = top_of(book);\n  if (have_time_ && m.time > last_time_) {"),
    ("L12 replay: fills buffer released after every execution (allocates per message)",
     "lobster/src/replayer.cpp",
     "      fills_.clear();\n      s = book_.execute_resting(id, m.size, m.time, fills_);",
     "      fills_.clear();\n      fills_.shrink_to_fit();\n"
     "      s = book_.execute_resting(id, m.size, m.time, fills_);"),
    ("L13 cut experiment: ghosts seeded from the N-level row 1, not the complete book",
     "lobster/tools/cut_full_seed.cpp", "rep.seed_ghosts(m, full)", "rep.seed_ghosts(m, row)"),
]


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", help="write the outcome table to this file")
    ap.add_argument("--timeout", type=int, default=900, help="per test program, seconds")
    args = ap.parse_args()

    repo = pathlib.Path(__file__).resolve().parents[2]
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="lob_lobster_mutation_"))
    pristine, work, build = tmp / "pristine", tmp / "work", tmp / "build"
    try:
        pristine.mkdir(parents=True)
        shutil.copy(repo / "CMakeLists.txt", pristine / "CMakeLists.txt")
        for name in ("common", "core", "lobster"):
            shutil.copytree(repo / name, pristine / name, ignore=shutil.ignore_patterns("build*"))
        shutil.copytree(pristine, work)

        gen = ["-G", "Ninja"] if shutil.which("ninja") else []
        r = run(["cmake", "-S", str(work), "-B", str(build), *gen, "-DCMAKE_BUILD_TYPE=Release",
                 "-DLOB_MODULES=core;lobster", "-DLOB_BUILD_BENCH=OFF"])
        if r.returncode != 0:
            sys.exit("cmake failed:\n" + r.stderr)

        def run_tests():
            failed = []
            for t in UNIT:
                try:
                    if run([str(build / "lobster" / t)], timeout=args.timeout).returncode != 0:
                        failed.append(t.replace("lobster_test_", ""))
                except subprocess.TimeoutExpired:
                    failed.append(t.replace("lobster_test_", "") + " (timeout)")
            cmd = [sys.executable, str(work / "lobster/tests/integration_test.py"),
                   "--replay", str(build / "lobster/lobster_replay"),
                   "--synth", str(work / "lobster/tools/synth_lobster.py"),
                   "--full-seed", str(build / "lobster/lobster_cut_full_seed"),
                   "--workdir", str(tmp / "integration")]
            try:
                if run(cmd, timeout=args.timeout).returncode != 0:
                    failed.append("integration")
            except subprocess.TimeoutExpired:
                failed.append("integration (timeout)")
            return failed

        # Sanity check first: the unmutated sources must build and pass.
        b = run(["cmake", "--build", str(build), "--target", *TARGETS])
        if b.returncode != 0:
            sys.exit("unmutated build failed:\n" + b.stdout[-2000:])
        if run_tests():
            sys.exit("unmutated sources fail the tests; fix them before running a mutation check")

        lines = []
        survivors = 0
        for name, rel, old, new in MUTANTS:
            shutil.copy(pristine / rel, work / rel)
            text = (work / rel).read_text()
            if text.count(old) != 1:
                sys.exit(f"{name}: expected text occurs {text.count(old)} times in {rel}")
            (work / rel).write_text(text.replace(old, new))
            t0 = time.time()
            b = run(["cmake", "--build", str(build), "--target", *TARGETS])
            if b.returncode != 0:
                lines.append(f"{name}: DOES NOT COMPILE (not a valid mutant)")
                survivors += 1
            else:
                failed = run_tests()
                if failed:
                    lines.append(f"{name}: caught by {', '.join(failed)}  "
                                 f"[{time.time() - t0:.0f} s]")
                else:
                    lines.append(f"{name}: SURVIVED")
                    survivors += 1
            shutil.copy(pristine / rel, work / rel)
            print(lines[-1], flush=True)

        head = run(["git", "rev-parse", "HEAD"], cwd=repo).stdout.strip() or "unknown"
        dirty = run(["git", "status", "--porcelain", "--", "lobster"], cwd=repo).stdout.strip()
        summary = [
            "# Generated by lobster/tests/mutation_check.py; do not edit by hand.",
            f"# git:  {head}" + ("  (lobster/ had uncommitted changes)" if dirty else ""),
            f"# date: {datetime.datetime.now(datetime.timezone.utc):%Y-%m-%dT%H:%M:%SZ}",
            "# Each mutant is one deliberate bug; 'caught' means at least one of format, replay,"
            " alloc (unit tests) and integration failed.",
            "",
            *lines,
            "",
            f"{len(MUTANTS) - survivors} of {len(MUTANTS)} mutants caught",
        ]
        if args.out:
            pathlib.Path(args.out).parent.mkdir(parents=True, exist_ok=True)
            pathlib.Path(args.out).write_text("\n".join(summary) + "\n")
        print(summary[-1])
        sys.exit(1 if survivors else 0)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
