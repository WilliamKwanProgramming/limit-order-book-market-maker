"""Builds the result tables of the backtest module from the CSV output of backtest/reproduce.sh.

Reads results/backtest/{params.csv, calibration/, baseline/, stress/}, writes
results/backtest/tables.md and splices the tables into backtest/README.md between the
<!-- params:begin/end --> and <!-- tables:begin/end --> markers. No number is typed by hand.
"""
from pathlib import Path

import numpy as np
import pandas as pd

ROOT = Path(__file__).resolve().parent.parent
RES = ROOT / "results" / "backtest"
README = ROOT / "backtest" / "README.md"


def kv(path):
    d = pd.read_csv(path)
    return dict(zip(d.key, d.value))


def run_field(run, key):
    for line in (RES / run / "run.txt").read_text().splitlines():
        if line.startswith(key + ":"):
            return line.split(":", 1)[1].strip()
    return "?"


CAL = kv(RES / "calibration" / "calibration.csv")
SIGMA, K, T = float(CAL["sigma"]), float(CAL["k"]), float(CAL["horizon"])


def skew_c(gamma):
    """Ticks by which one lot of inventory moves the reservation price at t = T/2."""
    return gamma * SIGMA**2 * T / 2


def label(row):
    if row.kind == "as":
        return f"A&S, c={skew_c(row.param):.2g}"
    if row.kind == "sym":
        return f"Symmetric, c={skew_c(row.param):.2g}"
    return {"naive": "NaiveTouch", "none": "None"}.get(row.kind, row.strategy)


def name_of(spec):
    kind, _, p = spec.partition(":")
    if kind in ("as", "sym"):
        return ("A&S" if kind == "as" else "Symmetric") + f", c={skew_c(float(p)):.2g}"
    return {"naive": "NaiveTouch", "none": "None"}.get(kind, spec)


def ci(est, lo, hi, fmt):
    return f"{est:{fmt}} [{lo:{fmt}}, {hi:{fmt}}]"


def md(header, rows):
    out = ["| " + " | ".join(header) + " |", "|" + "---|" * len(header)]
    out += ["| " + " | ".join(str(x) for x in r) + " |" for r in rows]
    return "\n".join(out)


def params_table():
    p = pd.read_csv(RES / "params.csv")
    rows = [[f"`--{r['name']}`", r["group"], f"{r['value']:g}", r["unit"], r["rationale"]]
            for _, r in p.iterrows()]
    return md(["Flag", "Group", "Default", "Unit", "Rationale (all values stylized)"], rows)


def diagnostics_table():
    cal = pd.read_csv(RES / "calibration" / "diagnostics.csv")
    st = pd.read_csv(RES / "stress" / "stress_diagnostics.csv")
    rows = []
    c = cal[cal.strategy == "none"].iloc[0]
    rows.append(["calibration (0, -)", "0", f"{c.mo_rate:.3f}", f"{c['sigma_0.1s']:.3f}",
                 f"{c.sigma_1s:.3f}", f"{c.sigma_10s:.3f}", f"{c.sigma_60s:.3f}",
                 f"{c.spread:.3f}", f"{c.touch_depth:.2f}", f"{c.two_sided:.4f}",
                 f"{c.mo_dispersion:.2f}"])
    for _, r in st[st.strategy == "none"].iterrows():
        cell = "(0, -)" if r.nu == 0 else f"({r.nu:g}, {r.n_br:g})"
        rows.append([f"stress {cell}", f"{r.informed_rate:.3f}", f"{r.mo_rate:.3f}",
                     f"{r['sigma_0.1s']:.3f}", f"{r.sigma_1s:.3f}", f"{r.sigma_10s:.3f}",
                     f"{r.sigma_60s:.3f}", f"{r.spread:.3f}", f"{r.touch_depth:.2f}",
                     f"{r.two_sided:.4f}", f"{r.mo_dispersion:.2f}"])
    return md(["Run (ν, n_br)", "Informed MO/s", "All MO/s", "σ 0.1 s", "σ 1 s", "σ 10 s",
               "σ 60 s", "Spread (ticks)", "Touch depth (lots)", "Two-sided", "MO var/mean"],
              rows)


def calibration_table():
    reach = pd.read_csv(RES / "calibration" / "reach.csv")
    probes = pd.read_csv(RES / "calibration" / "summary.csv")
    probes = probes[probes.kind == "fixed"]
    used = reach[reach.used == 1]
    rows = [[f"{r.delta:g}", f"{r.rate:.4f}", f"{float(CAL['A']) * np.exp(-K * r.delta):.4f}",
             int(r['count']), "yes" if r.used else "no"] for _, r in reach[reach.delta <= 4].iterrows()]
    t1 = md(["δ (ticks)", "λ(δ) measured (1/s per side)", "A e^{-kδ} fitted", "Orders", "In fit"],
            rows)
    rows2 = [[f"{r.param:g}", f"{r.quote_dist:.3f}", f"{r.fill_rate:.4f}",
              f"{float(CAL['A']) * np.exp(-K * r.quote_dist):.4f}"] for _, r in probes.iterrows()]
    t2 = md(["Probe offset (ticks)", "Mean distance from mid (ticks)", "Fill rate per quote-second",
             "A e^{-kδ} at that distance"], rows2)
    return t1, t2, len(used)


def baseline_table():
    s = pd.read_csv(RES / "baseline" / "summary.csv")
    s = s[s.kind != "none"]
    rows = []
    for _, r in s.iterrows():
        se = r.sd_pnl / np.sqrt(r.n)
        rows.append([label(r), f"{r.param:.3g}" if r.kind in ("as", "sym") else "-",
                     f"{r.mean_pnl:.1f} ± {1.96 * se:.1f}", f"{r.sd_pnl:.1f}", f"{r.sharpe:.2f}",
                     f"{r.sd_final_inv:.2f}", f"{r.mean_abs_inv:.2f}", f"{r.mean_fills:.0f}",
                     f"{r.fill_rate:.3f}", f"{r.quote_dist:.2f}", f"{r.spread_captured:.2f}",
                     f"{r['markout_0.1s']:+.2f}", f"{r.markout_1s:+.2f}", f"{r.markout_10s:+.2f}",
                     f"{r.fill_dispersion:.2f}"])
    return md(["Strategy", "γ", "Mean PnL (USD) ± 95%", "SD PnL (USD)", "Mean/SD", "SD q_T (lots)",
               "Mean abs q (lots)", "Fills", "Fills per quote-s", "Quote distance (ticks)",
               "Spread captured (ticks)", "Markout 0.1 s", "Markout 1 s", "Markout 10 s",
               "Fill var/mean"], rows), s


def comparison_table(path, prefix_cols=(), as_only=False):
    c = pd.read_csv(path)
    if as_only:
        c = c[c.a.str.startswith("as:")]
    keep = ["mean_pnl_diff", "sd_pnl_ratio", "sd_final_inv_ratio", "mean_abs_inv_diff",
            "sharpe_diff", "markout_10s_diff"]
    fmts = {"mean_pnl_diff": "+.1f", "sd_pnl_ratio": ".3f", "sd_final_inv_ratio": ".3f",
            "mean_abs_inv_diff": "+.2f", "sharpe_diff": "+.3f", "markout_10s_diff": "+.2f"}
    rows = []
    group = list(prefix_cols) + ["a", "b"]
    for key, g in c.groupby(group, sort=False):
        g = g.set_index("statistic")
        lead = [] if not prefix_cols else [
            "(0, -)" if key[0] == 0 else f"({key[0]:g}, {key[1]:g})"]
        rows.append(lead + [name_of(key[-2]), name_of(key[-1])] +
                    [ci(g.loc[s, "estimate"], g.loc[s, "ci_lo"], g.loc[s, "ci_hi"], fmts[s])
                     for s in keep])
    head = (["Cell (ν, n_br)"] if prefix_cols else []) + [
        "A", "B", "Mean PnL A − B (USD)", "SD PnL A / B", "SD q_T A / B", "Mean abs q A − B (lots)",
        "Mean/SD A − B", "Markout 10 s A − B (ticks)"]
    return md(head, rows), c


def stress_table():
    s = pd.read_csv(RES / "stress" / "stress_summary.csv")
    s = s[s.kind != "none"]
    rows = []
    for _, r in s.iterrows():
        cell = "(0, -)" if r.nu == 0 else f"({r.nu:g}, {r.n_br:g})"
        se = r.sd_pnl / np.sqrt(r.n)
        rows.append([cell, label(r), f"{r.mean_pnl:.1f} ± {1.96 * se:.1f}", f"{r.sd_pnl:.1f}",
                     f"{r.sharpe:.2f}", f"{r.sd_final_inv:.2f}", f"{r.mean_abs_inv:.2f}",
                     f"{r.mean_fills:.0f}", f"{r.fill_rate:.3f}", f"{r.spread_captured:.2f}",
                     f"{r.markout_1s:+.2f}", f"{r.markout_10s:+.2f}", f"{r.fill_dispersion:.2f}"])
    return md(["Cell (ν, n_br)", "Strategy", "Mean PnL (USD) ± 95%", "SD PnL (USD)", "Mean/SD",
               "SD q_T (lots)", "Mean abs q (lots)", "Fills", "Fills per quote-s",
               "Spread captured (ticks)", "Markout 1 s", "Markout 10 s", "Fill var/mean"], rows), s


def fmt_iv(c, a, b, stat, fmt, **cell):
    d = c[(c.a == a) & (c.b == b) & (c.statistic == stat)]
    for k, v in cell.items():
        d = d[np.isclose(d[k], v)]
    r = d.iloc[0]
    return ci(r.estimate, r.ci_lo, r.ci_hi, fmt)


def key_results():
    """Bullet list of the numbers quoted in the README's reading, computed from the CSVs."""
    b = pd.read_csv(RES / "baseline" / "summary.csv")
    bc = pd.read_csv(RES / "baseline" / "comparison.csv")
    st = pd.read_csv(RES / "stress" / "stress_summary.csv")
    sc = pd.read_csv(RES / "stress" / "stress_comparison.csv")
    reach = pd.read_csv(RES / "calibration" / "reach.csv")
    a_s = b[b.kind == "as"].sort_values("param")
    sym = b[b.kind == "sym"].sort_values("param")
    naive = b[b.kind == "naive"].iloc[0]
    g_lo, g_hi = a_s.strategy.iloc[0], a_s.strategy.iloc[-1]
    s_lo, s_hi = "sym:" + g_lo[3:], "sym:" + g_hi[3:]
    n_sym = sym[["mean_pnl", "sd_pnl", "mean_fills"]].round(9).drop_duplicates().shape[0]
    out = []
    out.append(
        f"- Uninformed, A&S against Symmetric at the same γ: SD PnL ratio "
        f"{fmt_iv(bc, g_lo, s_lo, 'sd_pnl_ratio', '.3f')} at c={skew_c(a_s.param.iloc[0]):.2g} "
        f"and {fmt_iv(bc, g_hi, s_hi, 'sd_pnl_ratio', '.3f')} at c={skew_c(a_s.param.iloc[-1]):.2g}; "
        f"mean PnL difference {fmt_iv(bc, g_lo, s_lo, 'mean_pnl_diff', '+.1f')} USD and "
        f"{fmt_iv(bc, g_hi, s_hi, 'mean_pnl_diff', '+.1f')} USD; SD q_T ratio "
        f"{fmt_iv(bc, g_lo, s_lo, 'sd_final_inv_ratio', '.3f')} and "
        f"{fmt_iv(bc, g_hi, s_hi, 'sd_final_inv_ratio', '.3f')}.")
    out.append(f"- The five Symmetric γ values produced {n_sym} distinct results (identical quotes "
               "after rounding to the tick grid).")
    out.append(
        f"- NaiveTouch: mean PnL {naive.mean_pnl:.1f} USD, SD {naive.sd_pnl:.1f} USD, mean/SD "
        f"{naive.sharpe:.2f}, {naive.mean_fills:.0f} fills per episode; A&S mean/SD ranges from "
        f"{a_s.sharpe.min():.2f} to {a_s.sharpe.max():.2f} over the γ grid.")
    out.append(
        f"- Uninformed 10 s markouts are positive for every strategy (smallest "
        f"{b[b.kind != 'none'].markout_10s.min():+.2f} ticks per lot).")
    s2 = st[st.kind != "none"]
    neg = s2.groupby(["nu", "n_br"]).mean_pnl.max()
    neg = [f"({nu:g}, {nb:g})" for (nu, nb), v in neg.items() if v < 0]
    out.append(f"- Stress cells where every strategy loses money on average: {', '.join(neg)}.")
    a_st = st[st.kind == "as"].strategy.iloc[0]
    s_st = "sym:" + a_st[3:]
    out.append(
        f"- Stress, A&S against Symmetric (c={skew_c(float(a_st[3:])):.2g}): SD q_T ratio "
        f"{fmt_iv(sc, a_st, s_st, 'sd_final_inv_ratio', '.3f', nu=0)} without informed flow and "
        f"{fmt_iv(sc, a_st, s_st, 'sd_final_inv_ratio', '.3f', nu=0.1, n_br=0.9)} at (0.1, 0.9); "
        f"SD PnL ratio {fmt_iv(sc, a_st, s_st, 'sd_pnl_ratio', '.3f', nu=0)} and "
        f"{fmt_iv(sc, a_st, s_st, 'sd_pnl_ratio', '.3f', nu=0.1, n_br=0.9)}; mean PnL "
        f"difference {fmt_iv(sc, a_st, s_st, 'mean_pnl_diff', '+.1f', nu=0)} USD and "
        f"{fmt_iv(sc, a_st, s_st, 'mean_pnl_diff', '+.1f', nu=0.1, n_br=0.9)} USD; mean |q| "
        f"difference {fmt_iv(sc, a_st, s_st, 'mean_abs_inv_diff', '+.2f', nu=0.1, n_br=0.9)} "
        f"lots at (0.1, 0.9).")
    asx = st[st.kind == "as"]
    nvx = st[st.kind == "naive"]
    out.append(
        f"- Fill count var/mean (1 s bins): A&S {asx.fill_dispersion.min():.2f} to "
        f"{asx.fill_dispersion.max():.2f}, NaiveTouch {nvx.fill_dispersion.min():.2f} to "
        f"{nvx.fill_dispersion.max():.2f} across the stress grid; spread captured per fill by A&S "
        f"stays between {asx.spread_captured.min():.2f} and {asx.spread_captured.max():.2f} ticks.")
    r05 = reach[np.isclose(reach.delta, 0.5)].iloc[0]
    out.append(
        f"- k fit at the touch: measured λ(0.5) = {r05.rate:.3f}/s against "
        f"{float(CAL['A']) * np.exp(-K * 0.5):.3f}/s from the fitted exponential.")
    return "**Measured results quoted in the reading below** (computed by make_tables.py):\n\n" + \
        "\n".join(out)


def main():
    n_base = run_field("baseline", "episodes")
    n_stress = run_field("stress", "episodes")
    n_cal = run_field("calibration", "episodes")
    seeds = {r: run_field(r, "seed") for r in ("calibration", "baseline", "stress")}
    t_diag = diagnostics_table()
    t_reach, t_probe, n_used = calibration_table()
    t_base, _ = baseline_table()
    t_cmp, _ = comparison_table(RES / "baseline" / "comparison.csv")
    t_cmp_s, _ = comparison_table(RES / "baseline" / "comparison.csv", as_only=True)
    t_stress, _ = stress_table()
    t_scmp, _ = comparison_table(RES / "stress" / "stress_comparison.csv", ("nu", "n_br"))
    t_scmp_s, _ = comparison_table(RES / "stress" / "stress_comparison.csv", ("nu", "n_br"),
                                   as_only=True)
    reps = pd.read_csv(RES / "baseline" / "comparison.csv").replicates.iloc[0]

    est = (f"**A&S estimates** (calibration run, {n_cal} episodes, seed {seeds['calibration']}): "
           f"σ = {SIGMA:.4f} ticks/√s (10 s returns), k = {K:.4f} per tick, "
           f"A = {float(CAL['A']):.4f} per second per side, R² of the log-linear fit "
           f"{float(CAL['k_fit_r2']):.3f} over {n_used} points.")
    parts = [
        f"**Table 1. Market diagnostics** (strategy None; {n_cal} episodes for the calibration "
        f"run, {n_stress} per stress cell, seed {seeds['stress']}). σ is realized volatility of the "
        "mid in ticks/√s at the given sampling interval; spread and touch depth are time averages "
        "of the book without our orders; two-sided is the fraction of time both sides are "
        "non-empty; MO var/mean is the dispersion of market-order counts per 1 s bin (1 for "
        "Poisson).",
        t_diag, est,
        "**Table 2a. k fit.** Rate per side of market orders whose deepest execution is at least "
        "δ ticks from the mid before the order.", t_reach,
        "**Table 2b. Fixed-offset probes** (our 1-lot quotes at mid ∓ offset, rounded away from "
        "the mid; same calibration run). Probes join the back of the queue, so their fill rate "
        "is below the reach-based λ(δ).", t_probe,
        f"**Table 3. Uninformed baseline** ({n_base} episodes per strategy, seed "
        f"{seeds['baseline']}, all strategies on the same episodes). c is the skew in ticks per "
        "lot at t = T/2; markouts and spread captured are in ticks per lot (negative = loss "
        "after the fill).", t_base,
        f"**Table 4. Paired comparisons, uninformed baseline**: estimate and paired "
        f"percentile-bootstrap 95% interval ({reps} replicates, same episode indices for A and "
        "B).", t_cmp,
        f"**Table 5. Stress grid** ({n_stress} episodes per strategy and cell, seed "
        f"{seeds['stress']}; cell (0, -) has no informed flow).", t_stress,
        "**Table 6. Paired comparisons across the stress grid** (same bootstrap).", t_scmp,
    ]
    body = "\n\n".join(parts) + "\n"
    # The README shows A&S comparisons only; tables.md also has Symmetric against NaiveTouch.
    short = "\n\n".join(p if p is not t_cmp and p is not t_scmp else
                         (t_cmp_s if p is t_cmp else t_scmp_s) for p in parts)
    short += ("\n\nSymmetric against NaiveTouch comparisons are in `results/backtest/tables.md`."
              "\n\n" + key_results() + "\n")
    (RES / "tables.md").write_text("# Backtest result tables (generated by backtest/make_tables.py)"
                                   "\n\n" + params_table() + "\n\n" + body + "\n" + key_results() + "\n")
    print("wrote", RES / "tables.md")

    text = README.read_text()
    text = splice(text, "params", params_table() + "\n")
    text = splice(text, "tables", short)
    README.write_text(text)
    print("updated", README)


def splice(text, name, content):
    b, e = f"<!-- {name}:begin -->", f"<!-- {name}:end -->"
    if b not in text or e not in text:
        return text
    head, rest = text.split(b, 1)
    _, tail = rest.split(e, 1)
    return head + b + "\n" + content + e + tail


if __name__ == "__main__":
    main()
