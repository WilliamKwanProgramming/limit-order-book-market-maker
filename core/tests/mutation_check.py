#!/usr/bin/env python3
"""Mutation check for the core test suite: do the tests notice deliberate bugs?

The script copies the sources the core tests need into a temporary directory, applies ONE
deliberate bug ("mutant") at a time, rebuilds the three core test programs and runs them. A mutant
is "caught" if at least one test program fails (or times out). A mutant that survives means the
suite has a blind spot, and the script then exits with status 1.

Usage: python3 core/tests/mutation_check.py [--out FILE] [--timeout SECONDS]
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

TESTS = ["core_test_order_book", "core_test_matching_differential", "core_test_replay_differential"]

# (name, file, text that must occur exactly once, replacement)
MUTANTS = [
    ("M1 matching: limit comparison uses >= instead of >", "core/src/order_book.cpp",
     "(side == Side::Bid ? lv.price > limit : lv.price < limit)",
     "(side == Side::Bid ? lv.price >= limit : lv.price <= limit)"),
    ("M2 replace: priority kept only when the size strictly shrinks", "core/src/order_book.cpp",
     "new_price == node.price && new_qty <= node.qty",
     "new_price == node.price && new_qty < node.qty"),
    ("M3 id index: backward-shift test excludes the slot itself", "core/include/lob/book_detail/id_index.hpp",
     "(i < j) ? (i < h && h <= j) : (i < h || h <= j)",
     "(i < j) ? (i < h && h < j) : (i < h || h <= j)"),
    ("M4 price levels: binary-search range is one short", "core/include/lob/book_detail/price_levels.hpp",
     "levels_.begin() + static_cast<std::ptrdiff_t>(i), k,",
     "levels_.begin() + static_cast<std::ptrdiff_t>(i) - 1, k,"),
    ("M5 execute_resting: reports the wrong remaining size", "core/src/order_book.cpp",
     "Fill{id, kNoOrder, node.side, node.price, take, ts, node.qty - take}",
     "Fill{id, kNoOrder, node.side, node.price, take, ts, node.qty}"),
    ("M6 matching: fills at the aggressor's limit price", "core/src/order_book.cpp",
     "Fill{node.id, aggressor, resting_side, lv.price, take, ts, node.qty}",
     "Fill{node.id, aggressor, resting_side, has_limit ? limit : lv.price, take, ts, node.qty}"),
    ("M7 reduce/replace: side total not updated", "core/src/order_book.cpp",
     "  levels(node.side).find(node.price)->qty -= by;\n  total_qty_[side_index(node.side)] -= by;\n",
     "  levels(node.side).find(node.price)->qty -= by;\n"),
    ("M8 queue: new orders join the front of their level", "core/src/order_book.cpp",
     "  node.prev = lv.tail;\n  if (lv.tail != kNil) {\n    pool_[lv.tail].next = n;\n  } else {\n"
     "    lv.head = n;\n  }\n  lv.tail = n;\n",
     "  node.prev = kNil;\n  node.next = lv.head;\n  if (lv.head != kNil) {\n    pool_[lv.head].prev = n;\n"
     "  } else {\n    lv.tail = n;\n  }\n  lv.head = n;\n"),
]


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", help="write the outcome table to this file")
    ap.add_argument("--timeout", type=int, default=600, help="per test program, seconds")
    args = ap.parse_args()

    repo = pathlib.Path(__file__).resolve().parents[2]
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="lob_mutation_"))
    pristine, work, build = tmp / "pristine", tmp / "work", tmp / "build"
    try:
        for name in ("CMakeLists.txt", "common", "core"):
            src = repo / name
            if src.is_dir():
                shutil.copytree(src, pristine / name, ignore=shutil.ignore_patterns("build*"))
            else:
                (pristine).mkdir(parents=True, exist_ok=True)
                shutil.copy(src, pristine / name)
        shutil.copytree(pristine, work)

        gen = ["-G", "Ninja"] if shutil.which("ninja") else []
        r = run(["cmake", "-S", str(work), "-B", str(build), *gen, "-DCMAKE_BUILD_TYPE=Release",
                 "-DLOB_MODULES=core", "-DLOB_BUILD_BENCH=OFF"])
        if r.returncode != 0:
            sys.exit("cmake failed:\n" + r.stderr)

        # Sanity check first: the unmutated sources must build and pass.
        b = run(["cmake", "--build", str(build), "--target", *TESTS])
        if b.returncode != 0:
            sys.exit("unmutated build failed:\n" + b.stderr[-2000:])
        for t in TESTS:
            if run([str(build / "core" / t)], timeout=args.timeout).returncode != 0:
                sys.exit(f"unmutated {t} fails; fix the tests before running a mutation check")

        lines = []
        survivors = 0
        for name, rel, old, new in MUTANTS:
            shutil.copy(pristine / rel, work / rel)
            text = (work / rel).read_text()
            if text.count(old) != 1:
                sys.exit(f"{name}: expected text occurs {text.count(old)} times in {rel}; update the script")
            (work / rel).write_text(text.replace(old, new))
            t0 = time.time()
            b = run(["cmake", "--build", str(build), "--target", *TESTS])
            if b.returncode != 0:
                lines.append(f"{name}: DOES NOT COMPILE (not a valid mutant)")
                survivors += 1
            else:
                failed = []
                for t in TESTS:
                    try:
                        if run([str(build / "core" / t)], timeout=args.timeout).returncode != 0:
                            failed.append(t.replace("core_test_", ""))
                    except subprocess.TimeoutExpired:
                        failed.append(t.replace("core_test_", "") + " (timeout)")
                if failed:
                    lines.append(f"{name}: caught by {', '.join(failed)}  [{time.time() - t0:.0f} s]")
                else:
                    lines.append(f"{name}: SURVIVED")
                    survivors += 1
            shutil.copy(pristine / rel, work / rel)
            print(lines[-1], flush=True)

        head = run(["git", "rev-parse", "HEAD"], cwd=repo).stdout.strip() or "unknown"
        summary = [
            "# Generated by core/tests/mutation_check.py; do not edit by hand.",
            f"# git:  {head}",
            f"# date: {datetime.datetime.now(datetime.timezone.utc):%Y-%m-%dT%H:%M:%SZ}",
            "# Each mutant is one deliberate bug; 'caught' means at least one core test program failed.",
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
