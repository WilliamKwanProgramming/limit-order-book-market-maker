"""Builds the result tables of the Avellaneda-Stoikov replication from the CSV output.

Reads results/paper/{sims1000,sims100000}/ and paper/reference/as2008_tables.csv, writes
results/paper/tables.md and results/paper/agreement.csv, and splices the tables into
paper/README.md between the <!-- tables:begin --> and <!-- tables:end --> markers.
No number in the output is typed by hand.
"""
from pathlib import Path

import pandas as pd

ROOT = Path(__file__).resolve().parent.parent
RES = ROOT / "results" / "paper"
README = ROOT / "paper" / "README.md"
NAMES = {"inventory": "Inventory", "symmetric": "Symmetric", "symmetric_tv": "Symmetric (time-matched)"}
STATS = ["mean_profit", "sd_profit", "mean_final_q", "sd_final_q"]


def load(run):
    d = RES / run
    return pd.read_csv(d / "summary.csv"), pd.read_csv(d / "comparison.csv")


def seed_of(run):
    for line in (RES / run / "run.txt").read_text().splitlines():
        if line.startswith("seed:"):
            return line.split(":", 1)[1].strip()
    return "?"


def fmt_ci(row, pct=False):
    if pct:
        return f"{100 * row.estimate:+.2f}% [{100 * row.ci_lo:+.2f}, {100 * row.ci_hi:+.2f}]"
    return f"{row.estimate:.3f} [{row.ci_lo:.3f}, {row.ci_hi:.3f}]"


def main():
    ref = pd.read_csv(ROOT / "paper" / "reference" / "as2008_tables.csv")
    s1k, c1k = load("sims1000")
    s100k, c100k = load("sims100000")
    blocks = pd.read_csv(RES / "sims100000" / "blocks.csv")
    n_large = int(s100k.n.iloc[0])
    gammas = list(ref.gamma.drop_duplicates())

    # Table 1: the paper's layout, paper versus this repository.
    head1 = ["| γ | Strategy | Source | Avg spread | Profit | Std(Profit) | Final q | Std(Final q) |",
             "|---|---|---|---|---|---|---|---|"]
    t1, t1_short = list(head1), list(head1)
    def row(g, strat, src, r, digits):
        line = (f"| {g:g} | {NAMES[strat]} | {src} | {r.mean_spread:.{digits}f} | "
                  f"{r.mean_profit:.1f} | {r.sd_profit:.1f} | {r.mean_final_q:.2f} | "
                  f"{r.sd_final_q:.1f} |")
        t1.append(line)
        if strat != "symmetric_tv":
            t1_short.append(line)
    for g in gammas:
        for strat in ["inventory", "symmetric", "symmetric_tv"]:
            p = ref[(ref.gamma == g) & (ref.strategy == strat)]
            if len(p):
                row(g, strat, "paper, 1000 sims", p.iloc[0], 2)
            row(g, strat, "here, 1000 sims", s1k[(s1k.gamma == g) & (s1k.strategy == strat)].iloc[0], 3)
            row(g, strat, f"here, {n_large:,} sims",
                s100k[(s100k.gamma == g) & (s100k.strategy == strat)].iloc[0], 3)

    # Table 2: paired bootstrap intervals, inventory versus each symmetric benchmark.
    head2 = ["| γ | Benchmark | Sims | Std(Profit) ratio | Std(Final q) ratio | Mean profit, relative difference |",
             "|---|---|---|---|---|---|"]
    t2, t2_short = list(head2), list(head2)
    for g in gammas:
        for base in ["symmetric", "symmetric_tv"]:
            for label, c, s in [("1000", c1k, s1k), (f"{n_large:,}", c100k, s100k)]:
                sel = c[(c.gamma == g) & (c.baseline == base)].set_index("statistic")
                line = (f"| {g:g} | {NAMES[base]} | {label} | {fmt_ci(sel.loc['sd_profit_ratio'])} | "
                        f"{fmt_ci(sel.loc['sd_final_q_ratio'])} | "
                        f"{fmt_ci(sel.loc['mean_profit_rel_diff'], pct=True)} |")
                t2.append(line)
                if base == "symmetric":
                    t2_short.append(line)

    # Agreement: is each paper number a plausible 1000-simulation draw from this model?
    # Reference value: the large-run estimate. Spread of a 1000-sim estimate: the standard
    # deviation of that statistic across the large run's independent blocks of 1000.
    rows = []
    wide = blocks.pivot_table(index=["gamma", "block"], columns="strategy", values=["sd_profit", "sd_final_q"])
    for g in gammas:
        for strat in ["inventory", "symmetric"]:
            p = ref[(ref.gamma == g) & (ref.strategy == strat)].iloc[0]
            big = s100k[(s100k.gamma == g) & (s100k.strategy == strat)].iloc[0]
            b = blocks[(blocks.gamma == g) & (blocks.strategy == strat)]
            for st in STATS:
                rows.append(dict(gamma=g, statistic=f"{st} ({strat})", paper=p[st], large_run=big[st],
                                 sd_of_1000_sim_estimate=b[st].std(ddof=1), blocks=len(b)))
        w = wide.loc[g]
        for st, label in [("sd_profit", "sd_profit ratio"), ("sd_final_q", "sd_final_q ratio")]:
            pi = ref[(ref.gamma == g) & (ref.strategy == "inventory")].iloc[0][st]
            ps = ref[(ref.gamma == g) & (ref.strategy == "symmetric")].iloc[0][st]
            r = w[(st, "inventory")] / w[(st, "symmetric")]
            bi = s100k[(s100k.gamma == g) & (s100k.strategy == "inventory")].iloc[0][st]
            bs = s100k[(s100k.gamma == g) & (s100k.strategy == "symmetric")].iloc[0][st]
            rows.append(dict(gamma=g, statistic=f"{label} (inventory / symmetric)", paper=pi / ps,
                             large_run=bi / bs, sd_of_1000_sim_estimate=r.std(ddof=1), blocks=len(r)))
    agree = pd.DataFrame(rows)
    agree["z"] = (agree.paper - agree.large_run) / agree.sd_of_1000_sim_estimate
    agree.to_csv(RES / "agreement.csv", index=False, float_format="%.6g")

    t3 = ["| γ | Statistic | Paper | Here (large run) | SD of a 1000-sim estimate | z |",
          "|---|---|---|---|---|---|"]
    for r in agree.itertuples():
        t3.append(f"| {r.gamma:g} | {r.statistic} | {r.paper:.3g} | {r.large_run:.3g} | "
                  f"{r.sd_of_1000_sim_estimate:.2g} | {r.z:+.1f} |")
    far = agree[agree.z.abs() > 2]
    worst = agree.loc[agree.z.abs().idxmax()]
    verdict = (f"Of the {len(agree)} paper statistics compared (means and standard deviations of "
               f"profit and final inventory, and the two standard-deviation ratios, for γ = "
               f"{', '.join(f'{g:g}' for g in gammas)}), {len(agree) - len(far)} lie within 2 "
               f"standard deviations of a 1000-simulation estimate from this model "
               f"(|z| ≤ 2). The largest deviation is {worst.statistic} at γ = {worst.gamma:g} "
               f"(paper {worst.paper:.3g}, here {worst.large_run:.3g}, z = {worst.z:+.1f}).")
    if len(far):
        verdict += " Outside |z| ≤ 2: " + "; ".join(
            f"{r.statistic} at γ = {r.gamma:g} (z = {r.z:+.1f})" for r in far.itertuples()) + "."

    seeds = (f"Seeds: 1000-sim run {seed_of('sims1000')}, {n_large:,}-sim run "
             f"{seed_of('sims100000')}.")
    reps = (f"{int(c1k.replicates.iloc[0])} replicates for 1000 sims, "
            f"{int(c100k.replicates.iloc[0])} for {n_large:,}")

    def compose(a, b):
        return "\n".join(
            [f"**Table 1.** Layout of the paper's tables (Section 3.3 of the author's PDF). {seeds}", ""] + a +
            ["", "**Table 2.** Inventory strategy relative to a symmetric benchmark: estimate and "
             f"paired percentile-bootstrap 95% interval ({reps}).", ""] + b +
            ["", "**Agreement with the paper.** " + verdict, ""])

    full = compose(t1, t2)
    body = compose(t1_short, t2_short)
    (RES / "tables.md").write_text(full + "\n**Table 3.** Paper value against this model "
                                   "(z = (paper - large run) / SD of a 1000-sim estimate).\n\n"
                                   + "\n".join(t3) + "\n")

    text = README.read_text()
    begin, end = "<!-- tables:begin -->", "<!-- tables:end -->"
    if begin in text and end in text:
        head, rest = text.split(begin, 1)
        _, tail = rest.split(end, 1)
        README.write_text(f"{head}{begin}\n{body}\n{end}{tail}")
    print(body)


if __name__ == "__main__":
    main()
