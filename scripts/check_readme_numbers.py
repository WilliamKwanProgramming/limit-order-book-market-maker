#!/usr/bin/env python3
"""Check that the headline numbers in README.md are the numbers in the committed results files.

Each entry of CHECKS names a results file, a text that must occur in that file (a table row, or a
number with its interval) and a text that must occur in README.md. When a result is regenerated and
its number changes, the first test fails; when the README is edited by hand and no longer quotes the
number, the second fails. An optional fourth element is the number of times the file text must occur.
It also fails when a relative link or image in README.md points to a file that does not exist.

Usage: python3 scripts/check_readme_numbers.py      (exit status 1 on any problem)
Standard library only.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
BT = "results/backtest/tables.md"
LOB = "results/lobster/tables.md"

CHECKS = [
    # Backtest, uninformed baseline: Avellaneda-Stoikov against symmetric quoting at c = 1.
    (BT, "| A&S, c=1 | Symmetric, c=1 | -14.9 [-18.0, -11.8] | 0.217 [0.205, 0.229] | 0.394 [0.381, 0.408] |",
     "0.217 [0.205, 0.229]"),
    (BT, "| A&S, c=1 | Symmetric, c=1 | -14.9 [-18.0, -11.8] | 0.217 [0.205, 0.229] | 0.394 [0.381, 0.408] |",
     "0.394 [0.381, 0.408]"),
    (BT, "| A&S, c=1 | Symmetric, c=1 | -14.9 [-18.0, -11.8] | 0.217 [0.205, 0.229] | 0.394 [0.381, 0.408] |",
     "-14.9 [-18.0, -11.8]"),
    (BT, "| A&S, c=1 | 0.00729 | 18.5 ± 0.7 | 14.9 | 1.24 |", "18.5 ± 0.7, 14.9, 1.24"),
    (BT, "| Symmetric, c=1 | 0.00729 | 33.4 ± 3.0 | 69.0 | 0.48 |", "33.4 ± 3.0, 69.0, 0.48"),
    (BT, "| NaiveTouch | - | 62.9 ± 3.4 | 76.6 | 0.82 |", "62.9 ± 3.4, 76.6, 0.82"),
    # Backtest, stress cell (news rate 0.1, branching ratio 0.9).
    (BT, "| (0.1, 0.9) | A&S, c=0.1 | -1046.6 ± 29.1 |", "-1046.6 ± 29.1"),
    (BT, "| (0.1, 0.9) | Symmetric, c=0.1 | -920.9 ± 27.8 |", "-920.9 ± 27.8"),
    (BT, "| (0.1, 0.9) | NaiveTouch | -765.6 ± 24.9 |", "-765.6 ± 24.9"),
    (BT, "| (0.1, 0.9) | A&S, c=0.1 | Symmetric, c=0.1 | -125.8 [-134.7, -115.8] | 1.047 [1.031, 1.063] |",
     "-125.8 [-134.7, -115.8]"),
    (BT, "| (0.1, 0.9) | A&S, c=0.1 | Symmetric, c=0.1 | -125.8 [-134.7, -115.8] | 1.047 [1.031, 1.063] |",
     "1.047 [1.031, 1.063]"),
    # Engine, LOBSTER replay, Python bindings: timings (they change whenever the benchmark is rerun).
    ("results/core/bench.txt", "1.614e+07", "1.614e+07"),
    (LOB, "| parse+replay | 2000000 | 4.32e+06 | 3.72e+06 | 3.1e+06 |", "3.72e+06"),
    (LOB, "| parse+replay+validate | 2000000 | 8.74e+05 | 8.05e+05 | 6.88e+05 |", "8.05e+05"),
    ("results/python/bench.txt", "msgs/s median 1.469e+07", "1.469e+07"),
    ("results/python/bench.txt", "714.1", "714.1"),
    # LOBSTER validation: six synthetic files, 500,000 rows each, no mismatching row.
    (LOB, "| 0 | 0 | 0 | 500000 | 0 |", "0 of 3,000,000", 6),
    # Mutation checks and the paper replication.
    ("results/core/mutation_check.txt", "8 of 8 mutants caught", "8 of 8 mutants caught"),
    ("results/lobster/mutation_check.txt", "12 of 12 mutants caught", "12 of 12 mutants caught"),
    ("results/python/mutation_check.txt", "12 of 12 mutants caught", "12 of 12 mutants caught"),
    ("results/paper/tables.md", "30 lie within 2 standard deviations", "30 of the 30 statistics compared"),
]


def broken_links(readme: str) -> list[str]:
    """Relative links and images in the README whose target file or directory does not exist."""
    missing = []
    for target in re.findall(r"\]\(([^)\s]+)\)", readme):
        if target.startswith(("http://", "https://", "#", "mailto:")):
            continue
        if not (ROOT / target.split("#", 1)[0]).exists():
            missing.append(target)
    return missing


def main() -> int:
    readme = (ROOT / "README.md").read_text(encoding="utf-8")
    texts: dict[str, str] = {}
    problems = 0
    for target in broken_links(readme):
        print(f"README.md links to {target!r}, which does not exist")
        problems += 1
    for check in CHECKS:
        path, in_file, in_readme = check[:3]
        expected = check[3] if len(check) > 3 else None
        if path not in texts:
            try:
                texts[path] = (ROOT / path).read_text(encoding="utf-8")
            except FileNotFoundError:
                print(f"{path}: file not found")
                problems += 1
                continue
        found = texts[path].count(in_file)
        if found == 0 or (expected is not None and found != expected):
            print(f"{path}: {in_file!r} occurs {found} times (expected "
                  f"{'at least 1' if expected is None else expected})")
            problems += 1
        if in_readme not in readme:
            print(f"README.md does not contain {in_readme!r} (checked against {path})")
            problems += 1
    print(f"{len(CHECKS)} checks, {problems} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
