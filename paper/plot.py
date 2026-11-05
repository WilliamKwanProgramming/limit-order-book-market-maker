"""Figures for the Avellaneda-Stoikov replication, written to results/paper/.

Inputs (from paper/reproduce.sh): results/paper/sims1000/{sims,trace}.csv and
results/paper/sweep/comparison.csv.
"""
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402
import pandas as pd  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
RES = ROOT / "results" / "paper"
# Fixed colour per strategy (validated categorical slots 1-3), never by rank.
COLOR = {"inventory": "#2a78d6", "symmetric": "#eb6834", "symmetric_tv": "#1baf7a"}
LABEL = {"inventory": "Inventory", "symmetric": "Symmetric",
         "symmetric_tv": "Symmetric (time-matched)"}
GAMMA = 0.1

plt.rcParams.update({
    "figure.dpi": 130, "axes.spines.top": False, "axes.spines.right": False,
    "axes.grid": True, "grid.color": "#e4e3df", "grid.linewidth": 0.6,
    "axes.edgecolor": "#8a8984", "axes.labelcolor": "#2b2a27", "xtick.color": "#52514e",
    "ytick.color": "#52514e", "font.size": 9, "legend.frameon": False,
})


def seed_of(run):
    for line in (RES / run / "run.txt").read_text().splitlines():
        if line.startswith("seed:"):
            return line.split(":", 1)[1].strip()
    return "?"


def save(fig, name):
    fig.tight_layout()
    fig.savefig(RES / name)
    plt.close(fig)
    print("wrote", RES / name)


def histograms(sims):
    d = sims[sims.gamma == GAMMA]
    n = d.sim.nunique()
    fig, ax = plt.subplots(figsize=(6, 3.4))
    bins = np.linspace(d.profit.min(), d.profit.max(), 50)
    for s in ["symmetric", "inventory"]:
        ax.hist(d[d.strategy == s].profit, bins=bins, color=COLOR[s], alpha=0.55,
                edgecolor="white", linewidth=0.4, label=LABEL[s])
    ax.set(xlabel="Profit at T (cash + q_T S_T)", ylabel="Simulations",
           title=f"Profit, γ = {GAMMA:g}, {n} simulations (seed {seed_of('sims1000')})")
    ax.legend()
    save(fig, "profit_hist_gamma0.1.png")

    fig, ax = plt.subplots(figsize=(6, 3.4))
    lo, hi = d.final_q.min(), d.final_q.max()
    bins = np.arange(lo - 0.5, hi + 1.5)
    for s in ["symmetric", "inventory"]:
        ax.hist(d[d.strategy == s].final_q, bins=bins, color=COLOR[s], alpha=0.55,
                edgecolor="white", linewidth=0.4, label=LABEL[s])
    ax.set(xlabel="Final inventory q_T", ylabel="Simulations",
           title=f"Final inventory, γ = {GAMMA:g}, {n} simulations (seed {seed_of('sims1000')})")
    ax.legend()
    save(fig, "final_q_hist_gamma0.1.png")


def sample_path(trace):
    t = trace[trace.gamma == GAMMA]
    inv = t[t.strategy == "inventory"]
    sym = t[t.strategy == "symmetric"]
    q = inv.iloc[:-1]  # the last row is the terminal state, without quotes
    fig, (a1, a2) = plt.subplots(2, 1, figsize=(7, 5), sharex=True,
                                 gridspec_kw={"height_ratios": [2, 1]})
    a1.plot(inv.t, inv.mid, color="#2b2a27", lw=1.2, label="Mid price s")
    a1.plot(q.t, q.ask, color=COLOR["inventory"], lw=0.9, label="Inventory strategy ask")
    a1.plot(q.t, q.bid, color=COLOR["inventory"], lw=0.9, ls="--", label="Inventory strategy bid")
    a1.scatter(q.t[q.ask_fill == 1], q.ask[q.ask_fill == 1], s=12, marker="v",
               color=COLOR["inventory"], label="Ask filled", zorder=3)
    a1.scatter(q.t[q.bid_fill == 1], q.bid[q.bid_fill == 1], s=12, marker="^",
               color=COLOR["inventory"], facecolor="white", label="Bid filled", zorder=3)
    a1.set(ylabel="Price")
    fig.suptitle(f"Simulation 0, γ = {GAMMA:g} (seed {seed_of('sims1000')})", fontsize=10)
    a1.legend(ncol=5, fontsize=7, loc="lower left", bbox_to_anchor=(0.0, 1.0))
    for s, d in [("inventory", inv), ("symmetric", sym)]:
        a2.step(d.t, d.q, where="post", color=COLOR[s], lw=1.2, label=LABEL[s])
    a2.axhline(0, color="#8a8984", lw=0.6)
    a2.set(xlabel="Time t", ylabel="Inventory q")
    a2.legend(fontsize=7, loc="upper left")
    save(fig, "sample_path_gamma0.1.png")


def sweep(comp):
    c = comp[comp.baseline == "symmetric"].sort_values("gamma")
    reps = int(c.replicates.iloc[0])
    fig, (a1, a2) = plt.subplots(1, 2, figsize=(8.5, 3.4))
    for stat, label, color in [("sd_profit_ratio", "Std(Profit) ratio", COLOR["inventory"]),
                               ("sd_final_q_ratio", "Std(Final q) ratio", COLOR["symmetric"])]:
        d = c[c.statistic == stat]
        a1.fill_between(d.gamma, d.ci_lo, d.ci_hi, color=color, alpha=0.2, lw=0)
        a1.plot(d.gamma, d.estimate, color=color, lw=1.6, marker="o", ms=3, label=label)
    a1.axhline(1, color="#8a8984", lw=0.6)
    a1.set(xscale="log", xlabel="Risk aversion γ", ylabel="Inventory / symmetric",
           title="Standard-deviation ratios", ylim=(0, 1.05))
    a1.legend()
    d = c[c.statistic == "mean_profit_rel_diff"]
    a2.fill_between(d.gamma, 100 * d.ci_lo, 100 * d.ci_hi, color=COLOR["inventory"], alpha=0.2, lw=0)
    a2.plot(d.gamma, 100 * d.estimate, color=COLOR["inventory"], lw=1.6, marker="o", ms=3)
    a2.axhline(0, color="#8a8984", lw=0.6)
    a2.set(xscale="log", xlabel="Risk aversion γ", ylabel="Percent",
           title="Mean profit, inventory vs symmetric")
    n = int(pd.read_csv(RES / "sweep" / "summary.csv").n.iloc[0])
    fig.suptitle(f"{n:,} paired simulations per γ (seed {seed_of('sweep')}); bands: bootstrap 95% CI, "
                 f"{reps} replicates", fontsize=8, color="#52514e")
    save(fig, "sweep_sd_ratio.png")


def main():
    histograms(pd.read_csv(RES / "sims1000" / "sims.csv"))
    sample_path(pd.read_csv(RES / "sims1000" / "trace.csv"))
    sweep(pd.read_csv(RES / "sweep" / "comparison.csv"))


if __name__ == "__main__":
    main()
