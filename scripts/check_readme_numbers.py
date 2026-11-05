#!/usr/bin/env python3
"""Check that the headline numbers in README.md are the numbers in the committed results files.

Each entry of CHECKS names a results file, a text that must occur in that file (a table row, or a
number with its interval) and a text that must occur in README.md. When a result is regenerated and
its number changes, the first test fails; when the README is edited by hand and no longer quotes the
number, the second fails. An optional fourth element is the number of times the file text must occur.
It also fails when a relative link or image in README.md points to a file that does not exist.
Tables in VERBATIM must appear line for line in a module README, and the benchmark table of
core/README.md is compared with results/core/bench.txt. The module READMEs (core, paper, lobster,
backtest, python) are checked more loosely: every number written with an interval, "x [lo, hi]",
or with a standard error, "x ± e", must occur in that form in a text file (.md or .txt) under
results/<module>/. PROSE lists further numbers written in the prose of README.md or a module README
(for example the skew shares and interaction of the backtest decomposition), each with the line of
a results file it comes from. Other numbers in the module READMEs' prose and the benchmark tables
typed into python/ are not checked.

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
    (BT, "| uninformed | 1 | skew first: SkewConst vs Symmetric (skew, without the time-varying spread) "
         "| -16.7 [-19.9, -13.8] | 0.216 [0.204, 0.229] |", "0.216 [0.204, 0.229]"),
    (BT, "| uninformed | 1 | spread first: SymTV vs Symmetric (spread term, without skew) "
         "| -5.3 [-7.4, -3.2] | 0.837 [0.807, 0.867] |", "0.837 [0.807, 0.867]"),
    (BT, "| uninformed | 1 | then A&S vs SkewConst (spread term, with skew) "
         "| +1.8 [+1.4, +2.3] | 1.004 [0.971, 1.035] |", "1.004 [0.971, 1.035]"),
    (BT, "| uninformed | 1 | 0.217 | 0.88 | 1.00 | 0.94 | 1.199 [1.145, 1.259] |",
     "1.199 [1.145, 1.259]"),
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
    ("results/core/bench_book_size.txt", "| 1,000,000 | 10,000 | 1,017,722; 19945 | 384.3 (2.60e+06) |", "2.60e+06 (384.3 ns per message)"),
    ("results/core/bench_book_size.txt", "| 300 | 20 | 311; 41 | 73.1 (1.37e+07) |", "1.37e+07 (73.1 ns) at 311 orders"),
    ("results/core/bench_book_size.txt", "| 10,000 | 100 | 10,184; 200 | 128.5 (7.78e+06) | 137.5 (7.28e+06) | 0.02 |", "at most 0.02 CPUs"),
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


# Numbers in README prose: (results file, text that must occur in it, README file, text that must
# occur in that README). As for CHECKS, the first fails when a regenerated result changes, the second
# when the prose no longer quotes it.
T7B_C01 = "| uninformed | 0.1 | 0.480 | 0.96 | 1.05 | 1.00 | 1.073 [1.049, 1.097] |"
T7B_C1 = "| uninformed | 1 | 0.217 | 0.88 | 1.00 | 0.94 | 1.199 [1.145, 1.259] |"
T7B_STRESS = "| stress (0.1, 0.9) | 0.1 | 1.047 | 1.27 | 1.03 | 1.15 |"
SKEWC_C1 = ("| uninformed | 1 | skew first: SkewConst vs Symmetric (skew, without the time-varying "
            "spread) | -16.7 [-19.9, -13.8] | 0.216 [0.204, 0.229] |")
PROSE: list[tuple[str, str, str, str]] = [
    # Top README, decomposition (Tables 7a and 7b): skew alone against A&S, the spread term with
    # and without skew, and the skew shares of the two orders at c = 1.
    (BT, SKEWC_C1, "README.md", "(0.216 against 0.217)"),
    (BT, "| uninformed | 1 | spread first: SymTV vs Symmetric (spread term, without skew) | -5.3 "
         "[-7.4, -3.2] | 0.837 [0.807, 0.867] |", "README.md", "without skew it lowers the ratio to 0.837"),
    (BT, "| uninformed | 1 | then A&S vs SkewConst (spread term, with skew) | +1.8 [+1.4, +2.3] | "
         "1.004 [0.971, 1.035] |", "README.md",
     "with skew it leaves the ratio unchanged (1.004 [0.971, 1.035]) and raises mean PnL "
     "(+1.8 [+1.4, +2.3] USD)"),
    (BT, T7B_C1, "README.md", "(0.88 for the skew when the spread term is added first, 1.00 when "
     "the skew is added first)"),
    # backtest/README.md reading: skew shares (both orders and their average) and the interaction.
    (BT, T7B_C01, "backtest/README.md", "0.96 spread first\n  and 1.05 skew first at $c = 0.1$"),
    (BT, T7B_C1, "backtest/README.md", "0.88 and 1.00 at $c = 1$"),
    (BT, T7B_C01, "backtest/README.md", "is 1.00 and 0.94"),
    (BT, T7B_C1, "backtest/README.md", "is 1.00 and 0.94"),
    (BT, T7B_STRESS, "backtest/README.md", "spread-first order (1.27)"),
    (BT, T7B_STRESS, "backtest/README.md", "skew first it is\n  1.03"),
    (BT, "| uninformed | 0.1 | A&S vs Symmetric (both) | -9.0 [-11.8, -6.2] | 0.480 [0.460, 0.503] |",
     "backtest/README.md", "against A&S/Symmetric 0.480 [0.460, 0.503]"),
    # Multiplicity: the number of pairs and intervals is the count make_tables.py writes.
    (BT, "152 pairs with 1216 intervals in the comparison files, and 15 2 x 2 sets with 45 "
         "interaction intervals", "backtest/README.md", "(152 pairs with 1216 intervals in the\n"
     "comparison files, and 15 2 x 2 sets with 45 interaction intervals in the factorial files;"),
]


# Tables written by a results script and pasted verbatim into a module README: every line of the
# first markdown table after the marker line of the results file must occur in the README.
VERBATIM: list[tuple[str, str, str]] = [
    ("results/core/bench_book_size.txt", "## Summary", "core/README.md"),
]


def table_after(text: str, marker: str) -> list[str]:
    """Lines of the first markdown table that follows the first line starting with `marker`."""
    lines = text.splitlines()
    start = next(i for i, ln in enumerate(lines) if ln.startswith(marker))
    rows: list[str] = []
    for ln in lines[start + 1:]:
        if ln.startswith("|"):
            rows.append(ln)
        elif rows:
            break
    return rows


def core_snapshot_problems() -> list[str]:
    """Rows of the benchmark table in core/README.md against results/core/bench.txt.

    Each row `| replay (...) | best | median | worst | median msgs/s |` must carry the best,
    median and worst ns per message printed under "<stream> timing over" in bench.txt, and the
    median rate to the three significant digits the README gives it.
    """
    readme = (ROOT / "core" / "README.md").read_text(encoding="utf-8")
    bench = (ROOT / "results" / "core" / "bench.txt").read_text(encoding="utf-8")
    problems = []
    for line in readme.splitlines():
        if not re.match(r"\| (replay|matching) ", line):
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cells) < 5 or not re.fullmatch(r"[\d.]+", cells[1]):
            continue  # another table whose first cell starts with the same word
        stream = cells[0].split()[0]
        try:
            block = bench.split(f"{stream} timing over", 1)[1]
            for key, cell in zip(("best", "median", "worst"), cells[1:4]):
                m = re.search(rf"^\s+{key}\s+([\d.]+)\s+([\d.e+]+)\s*$", block, re.M)
                if float(m.group(1)) != float(cell):
                    problems.append(f"core/README.md {stream} {key} {cell} ns/msg, "
                                    f"bench.txt has {m.group(1)}")
                if key == "median" and abs(float(m.group(2)) / float(cells[4]) - 1) > 0.006:
                    problems.append(f"core/README.md {stream} median rate {cells[4]}, "
                                    f"bench.txt has {m.group(2)}")
        except (IndexError, AttributeError, ValueError):
            problems.append(f"core/README.md row {line[:60]!r} cannot be matched to "
                            "results/core/bench.txt")
    return problems


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
    n_checks = len(CHECKS) + len(PROSE)
    for path, in_file, readme_file, in_readme in PROSE:
        for name in (path, readme_file):
            if name not in texts:
                try:
                    texts[name] = (ROOT / name).read_text(encoding="utf-8")
                except FileNotFoundError:
                    texts[name] = ""
        if in_file not in texts[path]:
            print(f"{path}: {in_file!r} not found")
            problems += 1
        if in_readme not in texts[readme_file]:
            print(f"{readme_file} does not contain {in_readme!r} (checked against {path})")
            problems += 1
    for results_file, marker, readme_file in VERBATIM:
        n_checks += 1
        try:
            rows = table_after((ROOT / results_file).read_text(encoding="utf-8"), marker)
            target = (ROOT / readme_file).read_text(encoding="utf-8")
        except (FileNotFoundError, StopIteration):
            print(f"{results_file} (marker {marker!r}) or {readme_file} not found")
            problems += 1
            continue
        for row in rows:
            if row not in target:
                print(f"{readme_file} does not contain the line {row!r} of the table in {results_file}")
                problems += 1
    n_checks += 1
    for problem in core_snapshot_problems():
        print(problem)
        problems += 1
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
