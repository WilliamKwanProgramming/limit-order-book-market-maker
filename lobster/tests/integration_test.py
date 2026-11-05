#!/usr/bin/env python3
"""lobster_integration: cross-implementation test of lobster_replay against synth_lobster.py.

1. For several seeds and N in {1, 5, 10}, generate 200,000 messages with the independent Python
   book and replay them with --book under the strict policy: requires exit code 0, zero
   anomalies, zero malformed rows and zero validation mismatches on every row.
2. Add 1 to one size in one order-book row: requires exactly one mismatching row, at that message,
   classified as size-only.
3. A file cut after 50,000 messages (--cut): strict without --bootstrap must stop at an unknown id;
   lenient with --bootstrap must have a mismatch rate at most half that of lenient without it
   (see CUT_RATIO). The measured rates are printed.

Data files are written under --workdir and deleted after use. Exit code 77 = skipped.
"""

import argparse
import os
import subprocess
import sys

MESSAGES = 200_000
CASES = [(1, 1), (2, 5), (3, 10), (4, 10)]  # (seed, levels)
CUT_CASE = (5, 10, 50_000)                   # (seed, levels, cut)
# Loose bound for the cut file: the bootstrapped mismatch rate must be at most CUT_RATIO times the
# rate without the bootstrap. The ghosts reproduce every visible level at the cut exactly, so what
# remains comes from orders deeper than N levels at the cut (invisible to the heuristic) and from
# ghost queue positions that differ from the real ones. The ratio measured when the test was
# written is printed below and is far under 0.5; the bound only guards against the heuristic
# becoming useless. In this toy flow orders turn over within about a thousand messages, so both
# rates are small; real books keep orders much longer.
CUT_RATIO = 0.5

failures = []


def check(cond, what):
    print(("  ok    " if cond else "  FAIL  ") + what)
    if not cond:
        failures.append(what)


def read_summary(path):
    out = {}
    with open(path) as f:
        for line in f:
            k, _, v = line.rstrip("\n").partition("=")
            out[k] = v
    return out


def generate(args, prefix, seed, levels, cut=0):
    cmd = [sys.executable, args.synth, "--messages", str(MESSAGES), "--levels", str(levels),
           "--seed", str(seed), "--out", prefix]
    if cut:
        cmd += ["--cut", str(cut)]
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
    return "%s_message_%d.csv" % (prefix, levels), "%s_orderbook_%d.csv" % (prefix, levels)


def replay(args, msgs, book, out, *extra):
    cmd = [args.replay, "--messages", msgs, "--book", book, "--out", out, "--quiet"] + list(extra)
    rc = subprocess.run(cmd).returncode
    return rc, read_summary(os.path.join(out, "summary.txt"))


def remove(*paths):
    for p in paths:
        if os.path.exists(p):
            os.remove(p)


def message_types(path):
    with open(path) as f:
        return [int(line.split(",")[1]) for line in f]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--replay", required=True)
    ap.add_argument("--synth", required=True)
    ap.add_argument("--workdir", required=True)
    args = ap.parse_args()
    if not os.path.exists(args.replay) or not os.path.exists(args.synth):
        print("skipped: missing %s or %s" % (args.replay, args.synth))
        return 77
    os.makedirs(args.workdir, exist_ok=True)

    for seed, levels in CASES:
        print("case seed=%d levels=%d messages=%d" % (seed, levels, MESSAGES))
        prefix = os.path.join(args.workdir, "s%d" % seed)
        msgs, book = generate(args, prefix, seed, levels)
        rc, s = replay(args, msgs, book, prefix + "_out")
        check(rc == 0, "exit code 0 (got %d)" % rc)
        check(s["rows_checked"] == str(MESSAGES), "rows checked = %s" % s["rows_checked"])
        check(s["anomalies"] == "0", "anomalies = %s" % s["anomalies"])
        check(s["rows_mismatched"] == "0", "rows mismatched = %s" % s["rows_mismatched"])
        check(s["message_parse_errors"] == "0" and s["book_parse_errors"] == "0",
              "malformed rows = %s + %s" % (s["message_parse_errors"], s["book_parse_errors"]))
        check(s["unchanged_row_violations"] == "0",
              "type 5/6/7 rows equal to the previous row (%s checked)"
              % s["unchanged_rows_checked"])
        if (seed, levels) == (3, 10):
            corruption_test(args, prefix, msgs, book)
        remove(msgs, book)

    cut_test(args)
    if failures:
        print("%d check(s) failed" % len(failures))
        return 1
    print("all checks passed")
    return 0


def corruption_test(args, prefix, msgs, book):
    types = message_types(msgs)
    # Message k (1-based) of type 1-4 followed by a type 1-4 message, so that the corrupted row is
    # not also the "previous row" of a type 5/6/7 row.
    k = next(i + 1 for i in range(100_000, len(types) - 1)
             if 1 <= types[i] <= 4 and 1 <= types[i + 1] <= 4)
    bad = prefix + "_corrupt_orderbook.csv"
    with open(book) as src, open(bad, "w", newline="\n") as dst:
        for i, line in enumerate(src, start=1):
            if i == k:
                fields = line.rstrip("\n").split(",")
                j = 1 if fields[1] != "0" else 3  # a non-empty size: ask or bid level 1
                fields[j] = str(int(fields[j]) + 1)
                line = ",".join(fields) + "\n"
            dst.write(line)
    print("corruption: size + 1 in order-book row %d" % k)
    rc, s = replay(args, msgs, bad, prefix + "_corrupt_out")
    check(rc == 1, "exit code 1 (got %d)" % rc)
    check(s["rows_mismatched"] == "1",
          "exactly one mismatching row (got %s)" % s["rows_mismatched"])
    check(s.get("first_mismatch_index") == str(k),
          "at message %d (got %s)" % (k, s.get("first_mismatch_index")))
    check(s["mismatch_size_only"] == "1", "classified as size-only")
    remove(bad)


def cut_test(args):
    seed, levels, cut = CUT_CASE
    print("cut: seed=%d levels=%d messages=%d cut=%d" % (seed, levels, MESSAGES, cut))
    prefix = os.path.join(args.workdir, "cut")
    msgs, book = generate(args, prefix, seed, levels, cut)
    rc, s = replay(args, msgs, book, prefix + "_strict")
    check(rc == 1 and s["stopped_early"] == "1", "strict without bootstrap stops (exit %d)" % rc)
    check("unknown_id" in s.get("first_anomaly_kinds", ""),
          "first anomaly is an unknown id (%s)" % s.get("first_anomaly_kinds"))
    _, plain = replay(args, msgs, book, prefix + "_lenient", "--policy", "lenient")
    _, boot = replay(args, msgs, book, prefix + "_bootstrap", "--policy", "lenient", "--bootstrap")
    rate_plain = int(plain["rows_mismatched"]) / int(plain["rows_checked"])
    rate_boot = int(boot["rows_mismatched"]) / int(boot["rows_checked"])
    print("  mismatch rate without bootstrap %.6f (%s rows), with bootstrap %.6f (%s rows)"
          % (rate_plain, plain["rows_mismatched"], rate_boot, boot["rows_mismatched"]))
    print("  ghosts created %s, references resolved %s, unexplained %s, shortfalls %s"
          % (boot["ghosts_created"], boot["ghost_references_resolved"],
             boot["ghost_unexplained"], boot["ghost_shortfall"]))
    check(rate_boot < rate_plain, "bootstrap lowers the mismatch rate")
    check(rate_boot <= CUT_RATIO * rate_plain,
          "bootstrap mismatch rate at most %.2f x the rate without it" % CUT_RATIO)
    print("  last mismatching message without bootstrap %s, with bootstrap %s"
          % (plain.get("last_mismatch_index"), boot.get("last_mismatch_index")))
    check(boot["ghost_seeded"] == "1" and int(boot["ghosts_created"]) > 0, "ghosts were seeded")
    remove(msgs, book)


if __name__ == "__main__":
    sys.exit(main())
