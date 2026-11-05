#!/usr/bin/env python3
"""Builds results/lobster/tables.md from the run outputs under results/lobster/ and copies the
tables into lobster/README.md between the markers <!-- tables:begin --> and <!-- tables:end -->
(Tables 1-5) and <!-- cut-full-seed:begin --> and <!-- cut-full-seed:end --> (Table 6, only if
results/lobster/cut_full_seed/ exists). Run by lobster/reproduce.sh and lobster/cut_full_seed.sh;
reads only files those scripts wrote."""

import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
RES = os.path.join(REPO, "results", "lobster")
README = os.path.join(REPO, "lobster", "README.md")


def kv(path):
    out = {}
    with open(path) as f:
        for line in f:
            k, sep, v = line.rstrip("\n").partition("=")
            if sep:
                out[k] = v
    return out


def gen_info(path):
    """generator.txt: key=value lines, plus 'seed=.. messages=.. levels=.. cut=..' on one line."""
    out = {}
    with open(path) as f:
        for line in f:
            for tok in line.split():
                k, sep, v = tok.partition("=")
                if sep:
                    out[k] = v
    return out


def pct(num, den):
    return "%.4f%%" % (100.0 * int(num) / int(den)) if int(den) else "n/a"


def validation_tables():
    base = os.path.join(RES, "validation")
    def order(n):  # "seed101_n5" -> (101, 5)
        seed, levels = n.split("_n")
        return int(seed[4:]), int(levels)

    names = sorted(os.listdir(base), key=order)
    t1 = ["| Seed | Messages | Levels N | Exit code | Malformed rows | Anomalies | Rows checked |"
          " Rows with a mismatch | Type 5/6/7 rows equal to previous |",
          "|---|---|---|---|---|---|---|---|---|"]
    t2 = ["| Seed | Levels N | Type 1 / 2 / 3 / 4 / 5 / 6 / 7 | Multi-row sweeps |"
          " Rows with an empty side | Rows with fewer than N levels | Rows sharing a timestamp |",
          "|---|---|---|---|---|---|---|"]
    t3 = ["| Seed | Levels N | Mean spread (price units) | Mean best bid size |"
          " Mean best ask size |"
          " Messages changing a best price | Executed vs bids / asks |",
          "|---|---|---|---|---|---|---|"]
    for n in names:
        s = kv(os.path.join(base, n, "summary.txt"))
        g = gen_info(os.path.join(base, n, "generator.txt"))
        seed, levels = g["seed"], g["levels"]
        malformed = int(s["message_parse_errors"]) + int(s["book_parse_errors"])
        ok_rows = int(s["unchanged_rows_checked"]) - int(s["unchanged_row_violations"])
        unchanged = "%d of %s" % (ok_rows, s["unchanged_rows_checked"])
        t1.append("| %s | %s | %s | %s | %d | %s | %s | %s | %s |" % (
            seed, s["message_rows"], levels, s["exit_code"], malformed, s["anomalies"],
            s["rows_checked"], s["rows_mismatched"], unchanged))
        types = " / ".join(s["type%d" % k] for k in range(1, 8))
        t2.append("| %s | %s | %s | %s | %s | %s | %s |" % (
            seed, levels, types, g["events_multi_row_sweeps"], g["rows_with_an_empty_side"],
            g["rows_with_fewer_than_n_levels"], g["rows_sharing_previous_timestamp"]))
        t3.append("| %s | %s | %.1f | %.1f | %.1f | %s | %s / %s |" % (
            seed, levels, float(s["stats_mean_spread_ticks"]), float(s["stats_mean_best_bid_qty"]),
            float(s["stats_mean_best_ask_qty"]), pct(s["stats_best_price_changes"], s["messages"]),
            s["stats_executed_bid"], s["stats_executed_ask"]))
    return t1, t2, t3


def cut_table():
    base = os.path.join(RES, "cut")
    names = sorted(os.listdir(base), key=lambda n: int(n.split("_n")[1]))
    t = ["| Levels N | Run | Messages processed | Unknown-id anomalies | Rows with a mismatch |"
         " Mismatch rate | Last mismatching message | Ghosts created / references resolved /"
         " unexplained / shortfalls |",
         "|---|---|---|---|---|---|---|---|"]
    for n in names:
        g = gen_info(os.path.join(base, n, "generator.txt"))
        for run, label in (("strict", "strict"), ("lenient", "lenient"),
                           ("lenient_bootstrap", "lenient + bootstrap")):
            s = kv(os.path.join(base, n, run, "summary.txt"))
            if s["stopped_early"] == "1":
                processed = "stopped at message %s" % s["first_anomaly_index"]
            else:
                processed = s["messages"]
            ghosts = "-"
            if s.get("ghost_seeded") == "1":
                ghosts = "%s / %s / %s / %s" % (s["ghosts_created"], s["ghost_references_resolved"],
                                                s["ghost_unexplained"], s["ghost_shortfall"])
            t.append("| %s | %s | %s | %s | %s | %s | %s | %s |" % (
                g["levels"], label, processed, s["anomaly_unknown_id"], s["rows_mismatched"],
                pct(s["rows_mismatched"], s["rows_checked"]), s.get("last_mismatch_index", "-"),
                ghosts))
    g = gen_info(os.path.join(base, names[0], "generator.txt"))
    caption = ("seed %s, %s messages written after %s simulated and dropped (--cut)"
               % (g["seed"], g["messages"], g["cut"]))
    return t, caption


def cut_full_seed_table():
    """results/lobster/cut_full_seed/ (lobster/cut_full_seed.sh), or None if it does not exist."""
    base = os.path.join(RES, "cut_full_seed")
    if not os.path.isdir(base):
        return None
    names = sorted((n for n in os.listdir(base) if os.path.isdir(os.path.join(base, n))),
                   key=lambda n: int(n.split("_n")[1]))
    t = ["| Levels N | Ghosts seeded from | Ghosts created (size) | References resolved |"
         " Unexplained / shortfalls | Unknown-id anomalies | Rows with a mismatch |"
         " Price structure / size only | Last mismatching message |",
         "|---|---|---|---|---|---|---|---|---|"]
    for n in names:
        g = gen_info(os.path.join(base, n, "generator.txt"))
        for run, label in (("lenient_bootstrap", "row 1 (N levels), `--bootstrap`"),
                           ("lenient_full_seed", "complete book at the cut")):
            s = kv(os.path.join(base, n, run, "summary.txt"))
            t.append("| %s | %s | %s (%s) | %s | %s / %s | %s | %s | %s / %s | %s |" % (
                g["levels"], label, s["ghosts_created"], s["ghost_created_qty"],
                s["ghost_references_resolved"], s["ghost_unexplained"], s["ghost_shortfall"],
                s["anomaly_unknown_id"], s["rows_mismatched"], s["mismatch_price_structure"],
                s["mismatch_size_only"], s.get("last_mismatch_index", "-")))
    g = gen_info(os.path.join(base, names[0], "generator.txt"))
    caption = ("**Table 6.** Synthetic cut files (seed %s, %s messages after a cut of %s), "
               "lenient policy, every row checked: bootstrap ghosts seeded from the first "
               "N-level order-book row against ghosts seeded from the generator's complete book."
               % (g["seed"], g["messages"], g["cut"]))
    return [caption, ""] + t


def bench_table():
    path = os.path.join(RES, "bench", "bench.txt")
    info = {}
    rows = ["| Stage | Messages | Best msgs/s | Median msgs/s | Worst msgs/s |",
            "|---|---|---|---|---|"]
    with open(path) as f:
        for line in f:
            m = re.match(r"(\S+): messages=(\d+) msgs_per_s best=(\S+) median=(\S+) worst=(\S+)",
                         line)
            if m:
                rows.append("| %s | %s | %.3g | %.3g | %.3g |" % (
                    m.group(1), m.group(2), float(m.group(3)), float(m.group(4)),
                    float(m.group(5))))
            else:
                k, _, v = line.rstrip("\n").partition("=")
                info[k] = v
    return rows, info


def main():
    t1, t2, t3 = validation_tables()
    tc, cut_caption = cut_table()
    tb, binfo = bench_table()
    lines = [
        "**Table 1.** Strict replay of synthetic files with `--book` (every row checked).",
        "",
    ] + t1 + [
        "",
        "**Table 2.** What the synthetic files contain (from the generator's own counters). For "
        "one seed the message stream does not depend on N; only the order-book file does.",
        "",
    ] + t2 + [
        "",
        "**Table 3.** `--stats` on the same files (toy flow; descriptive only). Spread in LOBSTER "
        "price units (1 = $0.0001; the toy grid is 100 units).",
        "",
    ] + t3 + [
        "",
        "**Table 4.** Window that starts mid-day: %s." % cut_caption,
        "",
    ] + tc + [
        "",
        "**Table 5.** Throughput on a 2,000,000-message synthetic file, N = 10, %s passes per "
        "stage (median of the passes; best and worst also shown). Build: %s. Load average %s "
        "before and %s after (shared 2-core VM)." % (
            binfo.get("bench_passes"), binfo.get("build"), binfo.get("load_average_before"),
            binfo.get("load_average_after")),
        "",
    ] + tb
    text = "\n".join(lines) + "\n"
    blocks = [("tables", text)]
    full = cut_full_seed_table()
    if full is not None:
        full_text = "\n".join(full) + "\n"
        blocks.append(("cut-full-seed", full_text))
        text += "\n" + full_text
    with open(os.path.join(RES, "tables.md"), "w") as f:
        f.write("<!-- generated by lobster/tools/make_tables.py; do not edit -->\n\n" + text)
    if os.path.exists(README):
        with open(README) as f:
            readme = f.read()
        for marker, block in blocks:
            readme, n = re.subn(r"(<!-- %s:begin -->\n).*?(<!-- %s:end -->)" % (marker, marker),
                                lambda m, b=block: m.group(1) + "\n" + b + "\n" + m.group(2),
                                readme, flags=re.S)
            if n != 1:
                print("README markers %s not found; table only in results/lobster/tables.md"
                      % marker, file=sys.stderr)
        with open(README, "w") as f:
            f.write(readme)
    return 0


if __name__ == "__main__":
    sys.exit(main())
