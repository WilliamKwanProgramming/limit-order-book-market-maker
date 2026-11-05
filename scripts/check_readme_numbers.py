#!/usr/bin/env python3
"""Check that the headline numbers in README.md are the numbers in the committed results files.

Each entry of CHECKS names a results file, a text that must occur in that file (a table row, or a
number with its interval) and a text that must occur in README.md. When a result is regenerated and
its number changes, the first test fails; when the README is edited by hand and no longer quotes the
number, the second fails. An optional fourth element is the number of times the file text must occur.
It also fails when a relative link or image in README.md points to a file that does not exist.
The module READMEs (core, paper, lobster, backtest, python) are checked more loosely: every number
written with an interval, "x [lo, hi]", or with a standard error, "x ± e", must occur in that
form in a text file (.md or .txt) under results/<module>/. Other numbers in their prose and the
benchmark tables typed into core/ and python/ are not checked.

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
    # Backtest decomposition (Table 7) and sensitivity to k and sigma (Table 9), interval check (Table 10).
    (BT, "| uninformed | 1 | A&S vs SymTV (skew) | -9.6 [-11.9, -7.0] | 0.259 [0.245, 0.272] |",
     "0.259 [0.245, 0.272]"),
    (BT, "| uninformed | 1 | SymTV vs Symmetric (spread term) | -5.3 [-7.4, -3.2] | 0.837 [0.807, 0.867] |",
     "0.837 [0.807, 0.867]"),
    (BT, "| uninformed | 1 | ×1 | ×1 | 18.1 ± 0.9 | 14.1 | 0.209 [0.194, 0.224] |", "0.209 [0.194, 0.224]"),
    (BT, "| uninformed | 1 | ×0.5 | ×1 | 12.8 ± 0.8 | 13.4 | 0.307 [0.279, 0.343] |", "0.307 [0.279, 0.343]"),
    (BT, "| uninformed | 1 | ×2 | ×1 | 15.9 ± 0.8 | 12.8 | 0.168 [0.154, 0.183] |", "0.168 [0.154, 0.183]"),
    (BT, "| stress (0.1, 0.9) | 0.1 | ×0.5 | ×1 | -747.5 ± 35.7 | 575.4 | 1.013 [0.994, 1.036] |",
     "1.013 [0.994, 1.036]"),
    (BT, "| stress (0.1, 0.9) | 0.1 | ×2 | ×1 | -1264.2 ± 45.5 | 733.7 | 1.160 [1.128, 1.193] |",
     "1.160 [1.128, 1.193]"),
    (BT, "Largest move: 3.5% of the width, within the 5% threshold", "| 3.5% |"),
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
    ("results/lobster/mutation_check.txt", "13 of 13 mutants caught", "13 of 13 mutants caught"),
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


MODULES = ("core", "paper", "lobster", "backtest", "python")
NUMBER = r"-?\d+(?:\.\d+)?(?:e[+-]?\d+)?"
QUOTED = re.compile(rf"{NUMBER} (?:\[{NUMBER}, {NUMBER}\]|± {NUMBER[1:]})")


def missing_quoted_numbers(module: str) -> tuple[int, list[str]]:
    """Intervals and standard errors written in <module>/README.md that no results text file has.

    Returns the number of distinct quoted items and the list of those not found, in the form they
    have in the README, among the .md and .txt files under results/<module>/.
    """
    readme = ROOT / module / "README.md"
    if not readme.exists():
        return 0, []
    quoted = sorted(set(QUOTED.findall(readme.read_text(encoding="utf-8"))))
    results = ""
    for f in sorted((ROOT / "results" / module).rglob("*")):
        if f.is_file() and f.suffix in (".md", ".txt"):
            results += f.read_text(encoding="utf-8", errors="replace")
    return len(quoted), [q for q in quoted if q not in results]


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
    n_checks = len(CHECKS)
    for module in MODULES:
        n_quoted, missing = missing_quoted_numbers(module)
        n_checks += 1
        for item in missing:
            print(f"{module}/README.md quotes {item!r}, which is in no .md or .txt file under "
                  f"results/{module}/")
            problems += 1
        if n_quoted:
            print(f"{module}/README.md: {n_quoted - len(missing)} of {n_quoted} quoted intervals and "
                  f"standard errors found in results/{module}/")
    print(f"{n_checks} checks, {problems} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
