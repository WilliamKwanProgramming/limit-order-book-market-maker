#!/usr/bin/env python3
"""Summarise several bench_replay runs (one per book size) as a markdown table.

Usage: book_size_table.py RUN1.txt [RUN2.txt ...]
Each file is the standard output of one `bench_replay --target-orders K --depth D --prefill 1 ...`
run. The table has one row per file, in the order given: the target book size, the mean number of
resting orders and price levels the generator's book held during the timed part, and the median
time per message (and messages per second) on the replay and matching streams.
Standard library only.
"""
import re
import sys


def parse(text: str) -> dict:
    out: dict = {}
    m = re.search(r"^book:\s+target (\d+) orders, depth (\d+) ticks", text, re.M)
    if not m:
        raise ValueError("no 'book:' line; is this bench_replay output from the book-size version?")
    out["target"], out["depth"] = int(m.group(1)), int(m.group(2))
    for name in ("replay", "matching"):
        m = re.search(rf"^{name} stream: .*?mean (\d+) orders.*?mean ([\d.]+) price levels", text, re.M)
        if not m:
            raise ValueError(f"no '{name} stream' line")
        out[name] = {"orders": int(m.group(1)), "levels": float(m.group(2))}
        m = re.search(rf"^{name} timing over (\d+) runs.*?^\s+median\s+([\d.]+)\s+([\d.e+]+)\s*$",
                      text, re.M | re.S)
        if not m:
            raise ValueError(f"no '{name} timing' block with a median line")
        out[name].update(runs=int(m.group(1)), ns=float(m.group(2)), rate=float(m.group(3)))
    return out


def table(rows: list[dict]) -> str:
    head = ("| Target orders | Depth (ticks) | Mean orders; mean price levels (replay stream) | "
            "Replay: median ns/msg (msgs/s) | Matching: median ns/msg (msgs/s) |")
    lines = [head, "|---|---|---|---|---|"]
    for r in rows:
        rp, mt = r["replay"], r["matching"]
        lines.append(
            f"| {r['target']:,} | {r['depth']:,} | {rp['orders']:,}; {rp['levels']:.0f} | "
            f"{rp['ns']:.1f} ({rp['rate']:.2e}) | {mt['ns']:.1f} ({mt['rate']:.2e}) |")
    return "\n".join(lines)


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(__doc__)
        return 2
    rows = []
    for path in argv[1:]:
        with open(path, encoding="utf-8") as f:
            rows.append(parse(f.read()))
    print(table(rows))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
