"""Figures for the backtest module, written to results/backtest/.

Inputs (from backtest/reproduce.sh): results/backtest/{calibration,baseline,stress}/*.csv.
"""
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402
import pandas as pd  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
RES = ROOT / "results" / "backtest"
# Same fixed colour per strategy family as paper/plot.py (validated categorical slots 1-3).
COLOR = {"as": "#2a78d6", "sym": "#eb6834", "naive": "#1baf7a"}
NAME = {"as": "A&S", "sym": "Symmetric", "naive": "NaiveTouch"}
MARKER = {0.5: "o", 0.75: "s", 0.9: "^"}

plt.rcParams.update({
    "figure.dpi": 130, "axes.spines.top": False, "axes.spines.right": False,
    "axes.grid": True, "grid.color": "#e4e3df", "grid.linewidth": 0.6,
    "axes.edgecolor": "#8a8984", "axes.labelcolor": "#2b2a27", "xtick.color": "#52514e",
    "ytick.color": "#52514e", "font.size": 9, "legend.frameon": False,
})

cal = pd.read_csv(RES / "calibration" / "calibration.csv")
CAL = dict(zip(cal.key, cal.value))
SIGMA, K, A, T = (float(CAL[x]) for x in ("sigma", "k", "A", "horizon"))


def c_of(gamma):
    return gamma * SIGMA**2 * T / 2


def seed(run):
    for line in (RES / run / "run.txt").read_text().splitlines():
        if line.startswith("seed:"):
            return line.split(":", 1)[1].strip()
    return "?"


def save(fig, name):
    fig.tight_layout()
    fig.savefig(RES / name)
    plt.close(fig)
    print("wrote", RES / name)


def frontier():
    s = pd.read_csv(RES / "baseline" / "summary.csv")
    n = int(s.n.iloc[0])
    fig, ax = plt.subplots(figsize=(6.4, 4.0))
    for kind in ("as", "sym"):
        d = s[s.kind == kind].sort_values("param")
        se = 1.96 * d.sd_pnl / np.sqrt(d.n)
        ax.errorbar(d.sd_pnl, d.mean_pnl, yerr=se, color=COLOR[kind], marker="o", ms=5, lw=1.5,
                    capsize=2, label=f"{NAME[kind]} (γ grid)")
        if kind == "as":
            for _, r in d.iterrows():
                ax.annotate(f"c={c_of(r.param):.2g}", (r.sd_pnl, r.mean_pnl),
                            textcoords="offset points", xytext=(-6, 5), ha="right", fontsize=7,
                            color="#52514e")
    d = s[s.kind == "naive"]
    ax.errorbar(d.sd_pnl, d.mean_pnl, yerr=1.96 * d.sd_pnl / np.sqrt(d.n), color=COLOR["naive"],
                marker="D", ms=6, capsize=2, ls="none", label="NaiveTouch")
    ax.set(xlabel="SD of episode PnL ($)", ylabel="Mean episode PnL ($), 95% CI",
           title=f"Uninformed market: PnL against risk ({n} episodes, seed {seed('baseline')})",
           xlim=(0, None))
    ax.legend(loc="lower right")
    save(fig, "frontier_uninformed.png")


def markouts():
    base = pd.read_csv(RES / "baseline" / "summary.csv")
    st = pd.read_csv(RES / "stress" / "stress_summary.csv")
    gmid = st[st.kind == "as"].param.iloc[0]
    hz = ["fill", "0.1 s", "1 s", "10 s"]
    cells = [("Uninformed baseline", base, None),
             ("Stress ν=0.03, n_br=0.75", st, (0.03, 0.75)),
             ("Stress ν=0.1, n_br=0.9", st, (0.1, 0.9))]
    fig, axes = plt.subplots(1, 3, figsize=(10, 3.4), sharey=False)
    for ax, (title, d, cell) in zip(axes, cells):
        if cell is not None:
            d = d[np.isclose(d.nu, cell[0]) & np.isclose(d.n_br, cell[1])]
        for kind in ("as", "sym", "naive"):
            r = d[(d.kind == kind) & ((kind == "naive") | np.isclose(d.param, gmid))]
            if r.empty:
                continue
            r = r.iloc[0]
            y = [r.spread_captured, r["markout_0.1s"], r.markout_1s, r.markout_10s]
            ax.plot(hz, y, color=COLOR[kind], marker="o", ms=4, lw=1.5, label=NAME[kind])
        ax.axhline(0, color="#8a8984", lw=0.8)
        ax.set(title=title, xlabel="Horizon after the fill")
    axes[0].set_ylabel("Markout (ticks per lot; < 0 = loss)")
    axes[0].legend()
    fig.suptitle(f"Per-fill markouts, A&S and Symmetric at c={c_of(gmid):.2g}", fontsize=9)
    save(fig, "markouts.png")


def stress():
    s = pd.read_csv(RES / "stress" / "stress_summary.csv")
    s = s[s.kind != "none"].copy()
    s["inf_rate"] = np.where(s.nu > 0, s.nu / (1 - s.n_br), np.nan)
    base = s[s.nu == 0]
    s = s[s.nu > 0]
    panels = [("mean_pnl", "Mean episode PnL ($)"), ("fill_rate", "Fills per quote-second"),
              ("markout_10s", "Markout at 10 s (ticks per lot)"),
              ("fill_dispersion", "Fill count var/mean (1 s bins)")]
    fig, axes = plt.subplots(2, 2, figsize=(9, 6.2))
    for ax, (col, lab) in zip(axes.flat, panels):
        for kind in ("as", "sym", "naive"):
            d = s[s.kind == kind].sort_values("inf_rate")
            ax.plot(d.inf_rate, d[col], color=COLOR[kind], lw=0.8, alpha=0.5)
            for nb, m in MARKER.items():
                e = d[np.isclose(d.n_br, nb)]
                ax.scatter(e.inf_rate, e[col], color=COLOR[kind], marker=m, s=22, zorder=3)
            b = base[base.kind == kind]
            ax.axhline(b[col].iloc[0], color=COLOR[kind], lw=0.8, ls=":")
        if col == "fill_dispersion":
            ax.axhline(1.0, color="#2b2a27", lw=0.8, ls="--")
            ax.annotate("Poisson", (s.inf_rate.min(), 1.0), xytext=(0, 3),
                        textcoords="offset points", fontsize=7, color="#52514e")
        ax.set(xscale="log", xlabel="Informed market orders per second, ν/(1 − n_br)", ylabel=lab)
    handles = [plt.Line2D([], [], color=COLOR[k], lw=1.5, label=NAME[k]) for k in COLOR]
    handles += [plt.Line2D([], [], color="#52514e", marker=m, ls="none", label=f"n_br={nb:g}")
                for nb, m in MARKER.items()]
    handles += [plt.Line2D([], [], color="#52514e", ls=":", lw=0.8, label="no informed flow")]
    fig.legend(handles=handles, loc="upper center", ncol=7, fontsize=8)
    gmid = s[s.kind == "as"].param.iloc[0]
    fig.suptitle(f"Stress grid ({int(s.n.iloc[0])} episodes per cell, seed {seed('stress')}; "
                 f"A&S and Symmetric at c={c_of(gmid):.2g})", fontsize=9, y=0.93)
    fig.tight_layout(rect=(0, 0, 1, 0.9))
    fig.savefig(RES / "stress.png")
    plt.close(fig)
    print("wrote", RES / "stress.png")


def calibration():
    reach = pd.read_csv(RES / "calibration" / "reach.csv")
    probes = pd.read_csv(RES / "calibration" / "summary.csv")
    probes = probes[probes.kind == "fixed"]
    fig, ax = plt.subplots(figsize=(6.0, 3.6))
    used = reach[reach.used == 1]
    ax.scatter(used.delta, used.rate, color="#2b2a27", s=16, label="market-order reach (in fit)")
    rest = reach[reach.used == 0]
    ax.scatter(rest.delta, rest.rate, facecolors="none", edgecolors="#8a8984", s=16,
               label="reach (too few orders, not fitted)")
    x = np.linspace(0.5, reach.delta.max(), 100)
    ax.plot(x, A * np.exp(-K * x), color=COLOR["as"], lw=1.5,
            label=f"fit A e^(-kδ): A={A:.3f}/s, k={K:.3f}/tick")
    ax.scatter(probes.quote_dist, probes.fill_rate, color=COLOR["sym"], marker="s", s=22,
               label="our 1-lot probes (fills per quote-second)")
    ax.set(yscale="log", xlabel="Distance from mid δ (ticks)", ylabel="Rate per side (1/s)",
           title=f"Calibration of k (uninformed market, seed {seed('calibration')})")
    ax.legend(fontsize=7)
    save(fig, "calibration_k.png")


if __name__ == "__main__":
    frontier()
    markouts()
    stress()
    calibration()
