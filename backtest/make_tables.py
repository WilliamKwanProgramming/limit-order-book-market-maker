"""Builds the result tables of the backtest module from the CSV output of backtest/reproduce.sh.

Reads results/backtest/{params.csv, calibration/, baseline/, stress/, mc_check/, sensitivity/},
writes results/backtest/tables.md and splices the tables into backtest/README.md between the
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


KIND_NAME = {"as": "A&S", "sym": "Symmetric", "symtv": "SymTV", "skewc": "SkewConst"}


def label(row):
    if row.kind in KIND_NAME:
        return f"{KIND_NAME[row.kind]}, c={skew_c(row.param):.2g}"
    return {"naive": "NaiveTouch", "none": "None"}.get(row.kind, row.strategy)


def name_of(spec):
    kind, _, p = spec.partition(":")
    if kind in KIND_NAME:
        return f"{KIND_NAME[kind]}, c={skew_c(float(p)):.2g}"
    return {"naive": "NaiveTouch", "none": "None"}.get(kind, spec)


def by_gamma(s):
    """Rows ordered NaiveTouch first, then A&S, SkewConst, SymTV, Symmetric for each gamma."""
    order = {"naive": 0, "as": 1, "skewc": 2, "symtv": 3, "sym": 4}
    s = s.assign(_g=np.where(s.kind == "naive", -1.0, s.param), _o=s.kind.map(order).fillna(9))
    return s.sort_values(["_g", "_o"], kind="stable").drop(columns=["_g", "_o"])


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


PM_HEAD = "Mean PnL (USD) ± 1.96 SE (normal approximation)"
AS_HEAD = "Adverse selection per fill (spread captured minus 10 s markout, ticks)"


def baseline_table():
    s = pd.read_csv(RES / "baseline" / "summary.csv")
    s = by_gamma(s[s.kind != "none"])
    rows = []
    for _, r in s.iterrows():
        se = r.sd_pnl / np.sqrt(r.n)
        rows.append([label(r), f"{r.param:.3g}" if r.kind in KIND_NAME else "-",
                     f"{r.mean_pnl:.1f} ± {1.96 * se:.1f}", f"{r.sd_pnl:.1f}", f"{r.sharpe:.2f}",
                     f"{r.sd_final_inv:.2f}", f"{r.mean_abs_inv:.2f}", f"{r.mean_fills:.0f}",
                     f"{r.fill_rate:.3f}", f"{r.quote_dist:.2f}", f"{r.spread_captured:.2f}",
                     f"{r['markout_0.1s']:+.2f}", f"{r.markout_1s:+.2f}", f"{r.markout_10s:+.2f}",
                     f"{r.fill_dispersion:.2f}", f"{r.spread_captured - r.markout_10s:.2f}"])
    return md(["Strategy", "γ", PM_HEAD, "SD PnL (USD)", "Mean/SD", "SD q_T (lots)",
               "Mean abs q (lots)", "Fills", "Fills per quote-s", "Quote distance (ticks)",
               "Spread captured (ticks)", "Markout 0.1 s", "Markout 1 s", "Markout 10 s",
               "Fill var/mean", AS_HEAD], rows), s


def comparison_table(path, prefix_cols=(), as_only=False, kept=False):
    c = pd.read_csv(path)
    # Pairs with the symtv or skewc arm are in the decomposition table.
    dec = ("symtv:", "skewc:")
    c = c[~c.a.str.startswith(dec) & ~c.b.str.startswith(dec)]
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
                     for s in keep] +
                    ([f"{int(g.loc[keep, 'replicates_kept'].min())} of {int(g.replicates.iloc[0])}"]
                     if kept else []))
    head = (["Cell (ν, n_br)"] if prefix_cols else []) + [
        "A", "B", "Mean PnL A − B (USD)", "SD PnL A / B", "SD q_T A / B", "Mean abs q A − B (lots)",
        "Mean/SD A − B", "Markout 10 s A − B (ticks)"] + (
        ["Bootstrap replicates kept (fewest over the columns)"] if kept else [])
    return md(head, rows), c


def stress_table():
    s = pd.read_csv(RES / "stress" / "stress_summary.csv")
    s = s[s.kind != "none"]
    s = pd.concat([by_gamma(g) for _, g in s.groupby(["nu", "n_br"], sort=False)])
    rows = []
    for _, r in s.iterrows():
        cell = "(0, -)" if r.nu == 0 else f"({r.nu:g}, {r.n_br:g})"
        se = r.sd_pnl / np.sqrt(r.n)
        rows.append([cell, label(r), f"{r.mean_pnl:.1f} ± {1.96 * se:.1f}", f"{r.sd_pnl:.1f}",
                     f"{r.sharpe:.2f}", f"{r.sd_final_inv:.2f}", f"{r.mean_abs_inv:.2f}",
                     f"{r.mean_fills:.0f}", f"{r.fill_rate:.3f}", f"{r.spread_captured:.2f}",
                     f"{r.markout_1s:+.2f}", f"{r.markout_10s:+.2f}", f"{r.fill_dispersion:.2f}",
                     f"{r.spread_captured - r.markout_10s:.2f}"])
    return md(["Cell (ν, n_br)", "Strategy", PM_HEAD, "SD PnL (USD)", "Mean/SD",
               "SD q_T (lots)", "Mean abs q (lots)", "Fills", "Fills per quote-s",
               "Spread captured (ticks)", "Markout 1 s", "Markout 10 s", "Fill var/mean",
               AS_HEAD], rows), s


def log_share(part_ratio, both_ratio):
    """ln(part) / ln(both): the share of ln(SD_AS / SD_Sym) carried by one step of a path."""
    return np.log(part_ratio) / np.log(both_ratio) if both_ratio != 1 else float("nan")


def fmt_share(x):
    return "undefined (ratio 1)" if np.isnan(x) else f"{x:.2f}"


DECOMP_STATS = [("mean_pnl_diff", "+.1f"), ("sd_pnl_ratio", ".3f"), ("sd_final_inv_ratio", ".3f"),
                ("sharpe_diff", "+.3f")]
# The 2 x 2 design: A&S (skew and time-varying spread), SkewConst (skew only), SymTV (time-varying
# spread only), Symmetric (neither). Each row is one paired comparison of Tables 4 and 6.
DECOMP_PAIRS = [("as", "sym", "A&S vs Symmetric (both)"),
                ("symtv", "sym", "spread first: SymTV vs Symmetric (spread term, without skew)"),
                ("as", "symtv", "then A&S vs SymTV (skew, with the time-varying spread)"),
                ("skewc", "sym",
                 "skew first: SkewConst vs Symmetric (skew, without the time-varying spread)"),
                ("as", "skewc", "then A&S vs SkewConst (spread term, with skew)")]
NA_TV = "n/a (SymTV = Symmetric)"
SAME_COLS = ["n", "mean_pnl", "sd_pnl", "sd_final_inv", "mean_abs_inv", "mean_fills", "fill_rate",
             "spread_captured", "markout_10s", "quote_dist"]


def cell_rows(df, cell):
    for k, v in cell.items():
        df = df[np.isclose(df[k], v)]
    return df


def decomposition(c, f, s, gamma, cell):
    """Both orders of the decomposition at one gamma and cell.

    c, f, s: comparison, factorial and summary tables (already restricted to the cell). Returns the
    five comparison rows (estimate and interval per statistic) and a dict with the shares and the
    interaction. The SD ratios satisfy A&S/Sym = A&S/SymTV x SymTV/Sym = A&S/SkewC x SkewC/Sym
    exactly; that is an algebraic identity (a/c = a/b x b/c), checked here only to confirm that the
    pairs use the same SDs, not evidence about the decomposition."""
    arm = {k: f"{k}:{gamma}" for k in ("as", "sym", "symtv", "skewc")}
    est, rows = {}, []
    for x, y, what in DECOMP_PAIRS:
        g = c[(c.a == arm[x]) & (c.b == arm[y])].set_index("statistic")
        est[(x, y)] = g.estimate
        rows.append((what, {k: ci(g.loc[k, "estimate"], g.loc[k, "ci_lo"], g.loc[k, "ci_hi"], fm)
                            for k, fm in DECOMP_STATS}))
    for k in ("sd_pnl_ratio", "sd_final_inv_ratio"):
        both = est[("as", "sym")][k]
        for first, second in ((("symtv", "sym"), ("as", "symtv")),
                              (("skewc", "sym"), ("as", "skewc"))):
            prod = est[first][k] * est[second][k]
            assert abs(prod / both - 1) < 1e-8, (gamma, cell, k, first)
    ia = f[(f["as"] == arm["as"]) & (f.skewc == arm["skewc"])].set_index("statistic")
    # The factorial file's point estimate must be the ratio of the two paired ratios.
    chk = est[("as", "skewc")]["sd_pnl_ratio"] / est[("symtv", "sym")]["sd_pnl_ratio"]
    assert abs(ia.loc["sd_pnl_interaction", "estimate"] / chk - 1) < 1e-8, (gamma, cell)
    tv, sy = (s[s.strategy == arm[k]].iloc[0] for k in ("symtv", "sym"))
    tv_is_sym = all(tv[k] == sy[k] for k in SAME_COLS)
    both = est[("as", "sym")]["sd_pnl_ratio"]
    spread_first = log_share(est[("as", "symtv")]["sd_pnl_ratio"], both)
    skew_first = log_share(est[("skewc", "sym")]["sd_pnl_ratio"], both)
    out = {"tv_is_sym": tv_is_sym, "spread_first": spread_first, "skew_first": skew_first,
           "shapley": 0.5 * (spread_first + skew_first), "both": both}
    for k, fm in (("sd_pnl_interaction", ".3f"), ("sd_final_inv_interaction", ".3f"),
                  ("mean_pnl_interaction", "+.1f")):
        out[k] = ci(ia.loc[k, "estimate"], ia.loc[k, "ci_lo"], ia.loc[k, "ci_hi"], fm)
        out[k + "_est"] = ia.loc[k, "estimate"]
    out["reps"] = int(ia.replicates.iloc[0])
    out["kept"] = int(ia.replicates_kept.min())
    return rows, out


def decomposition_cells(full):
    """(run name, c label, comparison, factorial, summary, gamma, cell) for Table 7.

    full=False: the README cells (baseline, stress (0, -) and (0.1, 0.9))."""
    bc = pd.read_csv(RES / "baseline" / "comparison.csv")
    bf = pd.read_csv(RES / "baseline" / "factorial.csv")
    bs = pd.read_csv(RES / "baseline" / "summary.csv")
    sc = pd.read_csv(RES / "stress" / "stress_comparison.csv")
    sf = pd.read_csv(RES / "stress" / "stress_factorial.csv")
    ss = pd.read_csv(RES / "stress" / "stress_summary.csv")
    out = []
    for g in sorted({a[3:] for a in bf["as"]}, key=float):
        out.append(("uninformed", f"{skew_c(float(g)):.2g}", bc, bf, bs, g, {}))
    for (nu, nb), _ in sf.groupby(["nu", "n_br"], sort=False):
        name = "stress (0, -)" if nu == 0 else f"stress ({nu:g}, {nb:g})"
        keep = nu == 0 or (np.isclose(nu, 0.1) and np.isclose(nb, 0.9))
        if not (full or keep):
            continue
        cell = {"nu": nu, "n_br": nb}
        g = cell_rows(sf, cell)["as"].iloc[0][3:]
        out.append((name, f"{skew_c(float(g)):.2g}", cell_rows(sc, cell), cell_rows(sf, cell),
                    cell_rows(ss, cell), g, cell))
    return out


def decomposition_table(full):
    """Table 7a (the five comparisons per cell) and Table 7b (shares and interaction)."""
    rows, rows2, res = [], [], {}
    for name, cl, c, f, s, g, _ in decomposition_cells(full):
        r, d = decomposition(c, f, s, g, {})
        res[(name, cl)] = d
        rows += [[name, cl, what] + [v[k] for k, _ in DECOMP_STATS] for what, v in r]
        na = d["tv_is_sym"]
        rows2.append([name, cl, f"{d['both']:.3f}",
                      NA_TV if na else fmt_share(d["spread_first"]), fmt_share(d["skew_first"]),
                      NA_TV if na else fmt_share(d["shapley"]), d["sd_pnl_interaction"],
                      d["sd_final_inv_interaction"], d["mean_pnl_interaction"]])
    t1 = md(["Run", "c", "A vs B", "Mean PnL A − B (USD)", "SD PnL A / B", "SD q_T A / B",
             "Mean/SD A − B"], rows)
    t2 = md(["Run", "c", "SD PnL A&S / Symmetric", "Skew share, spread first",
             "Skew share, skew first", "Skew share, average of the two orders (Shapley)",
             "Interaction, SD PnL: (A&S/SkewConst) / (SymTV/Symmetric)", "Interaction, SD q_T",
             "Interaction, mean PnL (USD): (A&S − SkewConst) − (SymTV − Symmetric)"], rows2)
    return t1, t2, res


SETTINGS = [("1", "1"), ("0.5", "1"), ("2", "1"), ("1", "0.8"), ("1", "1.25")]
PERTURB = {"k": (0.5, 2.0), "sigma": (0.8, 1.25)}


def blocks_table():
    """Table 8: sigma and k on blocks of the calibration run, and their spread."""
    b = pd.read_csv(RES / "calibration" / "calibration_blocks.csv")
    n = len(b)
    rows = [[int(r.block), f"{int(r.first_episode)}-{int(r.first_episode + r.episodes - 1)}",
             f"{r.sigma:.4f}", f"{r.k:.4f}", int(r.k_points)] for _, r in b.iterrows()]
    t1 = md(["Block", "Episodes", "σ̂ (ticks/√s)", "k̂ (1/tick)", "k-fit points"], rows)
    stats = {"blocks": n, "per_block": int(b.episodes.iloc[0])}
    rows2 = []
    for col, full, name in (("sigma", SIGMA, "σ̂"), ("k", K, "k̂")):
        sd = b[col].std(ddof=1)
        se = sd / np.sqrt(n)
        lo, hi = PERTURB[col]
        stats[col] = {"mean": b[col].mean(), "sd": sd, "se": se, "full": full,
                      "z_lo": abs(1 - lo) * full / se, "z_hi": abs(hi - 1) * full / se}
        s = stats[col]
        rows2.append([name, f"{full:.4f}", f"{s['mean']:.4f}", f"{sd:.4f}",
                      f"{b[col].min():.4f} to {b[col].max():.4f}", f"{se:.4f} ({100 * se / full:.1f}%)",
                      f"×{lo:g}: {s['z_lo']:.0f} SE; ×{hi:g}: {s['z_hi']:.0f} SE"])
    n_cal = run_field("calibration", "episodes")
    t2 = md(["Estimate", f"Full run ({n_cal} episodes)", f"Mean of {n} blocks", "SD of blocks",
             "Block range", f"SE of full estimate = SD/√{n} (% of estimate)",
             "Perturbation used, in SE"], rows2)
    return t1, t2, stats


def sensitivity_table(full):
    """Table 9: A&S and the paired SD-of-PnL ratios under perturbed k and sigma."""
    rows = []
    specs = [("uninformed", "uninformed", 2), ("uninformed", "uninformed", 4),
             ("stress_0.1_0.9", "stress (0.1, 0.9)", 2)]
    gam = {}
    for d, name, gi in specs:
        for ks, ss in SETTINGS:
            run = RES / "sensitivity" / f"{d}_k{ks}_s{ss}"
            s = pd.read_csv(run / "summary.csv")
            c = pd.read_csv(run / "comparison.csv")
            g = sorted(s[s.kind == "as"].param.unique())[0 if gi == 2 else -1]
            r = s[(s.kind == "as") & np.isclose(s.param, g)].iloc[0]
            gs = r.strategy[3:]
            gam[(name, gi)] = gs
            a, tv, sy = f"as:{gs}", f"symtv:{gs}", f"sym:{gs}"
            se = r.sd_pnl / np.sqrt(r.n)
            row = [name, f"{skew_c(g):.2g}", f"×{ks}", f"×{ss}",
                   f"{r.mean_pnl:.1f} ± {1.96 * se:.1f}", f"{r.sd_pnl:.1f}",
                   fmt_iv(c, a, sy, "sd_pnl_ratio", ".3f"), fmt_iv(c, a, tv, "sd_pnl_ratio", ".3f"),
                   fmt_iv(c, tv, sy, "sd_pnl_ratio", ".3f")]
            if full:
                row += [fmt_iv(c, a, sy, "sd_final_inv_ratio", ".3f"),
                        fmt_iv(c, a, sy, "mean_pnl_diff", "+.1f")]
            rows.append(row)
    head = ["Cell", "c", "k scale", "σ scale", "A&S " + PM_HEAD.replace("Mean", "mean"),
            "A&S SD PnL (USD)", "SD PnL A&S / Symmetric",
            "SD PnL A&S / SymTV (skew, with the time-varying spread)",
            "SD PnL SymTV / Symmetric (spread term, without skew)"]
    if full:
        head += ["SD q_T A&S / Symmetric", "Mean PnL A&S − Symmetric (USD)"]
    return md(head, rows)


def sensitivity_bullets():
    out = []
    for d, name, gi in [("uninformed", "uninformed", 2), ("uninformed", "uninformed", 4),
                        ("stress_0.1_0.9", "stress (0.1, 0.9)", 2)]:
        vals = {}
        for ks, ss in SETTINGS:
            run = RES / "sensitivity" / f"{d}_k{ks}_s{ss}"
            s = pd.read_csv(run / "summary.csv")
            c = pd.read_csv(run / "comparison.csv")
            g = sorted(s[s.kind == "as"].param.unique())[0 if gi == 2 else -1]
            gs = s[(s.kind == "as") & np.isclose(s.param, g)].strategy.iloc[0][3:]
            vals[(ks, ss)] = (g, fmt_iv(c, f"as:{gs}", f"sym:{gs}", "sd_pnl_ratio", ".3f"),
                              fmt_iv(c, f"as:{gs}", f"symtv:{gs}", "sd_pnl_ratio", ".3f"))
        g = vals[("1", "1")][0]
        parts = "; ".join(f"k×{ks}, σ×{ss}: {v[1]}" for (ks, ss), v in vals.items())
        out.append(f"- Sensitivity, {name}, c={skew_c(g):.2g}, SD PnL ratio A&S/Symmetric "
                   f"({run_field(f'sensitivity/{d}_k1_s1', 'episodes')} episodes): {parts}.")
    return out


def mc_table():
    """Table 10: headline intervals with the default replicate count and with 10,000."""
    checks = [("baseline/comparison.csv", "mc_check/baseline_c1/comparison.csv", "Table 4, c=1",
               "sd_pnl_ratio", "SD PnL A&S / Symmetric", ".4f", {}),
              ("baseline/comparison.csv", "mc_check/baseline_c1/comparison.csv", "Table 4, c=1",
               "sd_final_inv_ratio", "SD q_T A&S / Symmetric", ".4f", {}),
              ("stress/stress_comparison.csv", "mc_check/stress_0.1_0.9/comparison.csv",
               "Table 6, (0.1, 0.9)", "mean_pnl_diff", "Mean PnL A&S − Symmetric (USD)", ".2f",
               {"nu": 0.1, "n_br": 0.9})]
    rows, worst = [], 0.0
    # The check runs must reproduce the main runs' summaries for the two arms (same seeds).
    for main, chk in (("baseline/summary.csv", "mc_check/baseline_c1/summary.csv"),
                      ("stress/stress_summary.csv", "mc_check/stress_0.1_0.9/summary.csv")):
        m, c = pd.read_csv(RES / main), pd.read_csv(RES / chk)
        if "nu" in m:
            m = m[np.isclose(m.nu, 0.1) & np.isclose(m.n_br, 0.9)]
        for _, r in c.iterrows():
            mr = m[m.strategy == r.strategy].iloc[0]
            for col in ("mean_pnl", "sd_pnl", "sd_final_inv", "mean_fills"):
                assert mr[col] == r[col], (chk, r.strategy, col)
    for main, chk, where, stat, what, f, cell in checks:
        mc = pd.read_csv(RES / chk)
        a, b = mc.a.iloc[0], mc.b.iloc[0]
        m = pd.read_csv(RES / main)
        for k, v in cell.items():
            m = m[np.isclose(m[k], v)]
        r1 = m[(m.a == a) & (m.b == b) & (m.statistic == stat)].iloc[0]
        r2 = mc[(mc.a == a) & (mc.b == b) & (mc.statistic == stat)].iloc[0]
        width = r2.ci_hi - r2.ci_lo
        mv_lo, mv_hi = abs(r1.ci_lo - r2.ci_lo) / width, abs(r1.ci_hi - r2.ci_hi) / width
        worst = max(worst, mv_lo, mv_hi)
        rows.append([where, what, f"{r1.estimate:{f}}",
                     f"[{r1.ci_lo:{f}}, {r1.ci_hi:{f}}] ({int(r1.replicates)}, kept {int(r1.replicates_kept)})",
                     f"[{r2.ci_lo:{f}}, {r2.ci_hi:{f}}] ({int(r2.replicates)}, kept {int(r2.replicates_kept)})",
                     f"{100 * mv_lo:.1f}%, {100 * mv_hi:.1f}%"])
    t = md(["Interval", "Statistic", "Estimate", "Main run: interval (replicates)",
            "Check run: interval (replicates)",
            "Endpoint moves, lower and upper, % of the check interval's width"], rows)
    return t, worst


def dropped_replicates():
    """Bootstrap intervals in the comparison and factorial files, and those with dropped replicates.

    Returns (pairs, intervals in the comparison files, 2 x 2 sets, intervals in the factorial
    files, list of intervals with fewer kept replicates than drawn)."""
    files = sorted(RES.glob("*/comparison.csv")) + sorted(RES.glob("*/stress_comparison.csv")) + \
        sorted(RES.glob("*/*/comparison.csv"))
    ffiles = sorted(RES.glob("*/factorial.csv")) + sorted(RES.glob("*/stress_factorial.csv"))
    n = {"pairs": 0, "intervals": 0, "sets": 0, "set_intervals": 0}
    short = []
    for f in files + ffiles:
        c = pd.read_csv(f)
        fact = f in ffiles
        key = [k for k in ("nu", "n_br") if k in c] + (["as", "skewc"] if fact else ["a", "b"])
        n["sets" if fact else "pairs"] += len(c.drop_duplicates(key))
        n["set_intervals" if fact else "intervals"] += len(c)
        d = c[c.replicates_kept < c.replicates]
        short += [f"{f.relative_to(ROOT)}: {(r['as'] + ' set') if fact else (r.a + ' vs ' + r.b)} "
                  f"{r.statistic} ({r.replicates_kept} of {r.replicates})" for _, r in d.iterrows()]
    return n, short


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
    # Decomposition (Table 7): the cells named in the pre-specified reading, both orders.
    _, _, dec = decomposition_table(full=False)
    cells = dict(((name, cl), (c, g, cell)) for name, cl, c, _, _, g, cell in
                 decomposition_cells(full=False))
    for name, cl in [("uninformed", "0.1"), ("uninformed", "1"), ("stress (0, -)", "0.1"),
                     ("stress (0.1, 0.9)", "0.1")]:
        c, g, _ = cells[(name, cl)]
        d = dec[(name, cl)]

        def iv(x, y, stat, fm):
            return fmt_iv(c, f"{x}:{g}", f"{y}:{g}", stat, fm)

        s1 = NA_TV if d["tv_is_sym"] else fmt_share(d["spread_first"])
        s3 = NA_TV if d["tv_is_sym"] else fmt_share(d["shapley"])
        out.append(
            f"- Decomposition, {name}, c={cl}: SD PnL ratio A&S/Symmetric "
            f"{iv('as', 'sym', 'sd_pnl_ratio', '.3f')}. Spread first: SymTV/Symmetric (spread "
            f"term, without skew) {iv('symtv', 'sym', 'sd_pnl_ratio', '.3f')}, then A&S/SymTV (skew, "
            f"with the time-varying spread) {iv('as', 'symtv', 'sd_pnl_ratio', '.3f')}. Skew first: "
            f"SkewConst/Symmetric (skew, without the time-varying spread) "
            f"{iv('skewc', 'sym', 'sd_pnl_ratio', '.3f')}, then A&S/SkewConst (spread term, with "
            f"skew) {iv('as', 'skewc', 'sd_pnl_ratio', '.3f')}. Skew share: {s1} spread first, "
            f"{fmt_share(d['skew_first'])} skew first, {s3} on average. Interaction "
            f"(A&S/SkewConst)/(SymTV/Symmetric): SD PnL {d['sd_pnl_interaction']}, SD q_T "
            f"{d['sd_final_inv_interaction']}, mean PnL {d['mean_pnl_interaction']} USD. SD q_T "
            f"ratio: SymTV/Symmetric {iv('symtv', 'sym', 'sd_final_inv_ratio', '.3f')}, "
            f"A&S/SkewConst {iv('as', 'skewc', 'sd_final_inv_ratio', '.3f')}, SkewConst/Symmetric "
            f"{iv('skewc', 'sym', 'sd_final_inv_ratio', '.3f')}. Mean PnL difference (USD): "
            f"SymTV − Symmetric {iv('symtv', 'sym', 'mean_pnl_diff', '+.1f')}, A&S − SkewConst "
            f"{iv('as', 'skewc', 'mean_pnl_diff', '+.1f')}, SkewConst − Symmetric "
            f"{iv('skewc', 'sym', 'mean_pnl_diff', '+.1f')}.")
    out += sensitivity_bullets()
    n, short = dropped_replicates()
    out.append(f"- Bootstrap intervals: {n['pairs']} pairs with {n['intervals']} intervals in "
               f"the comparison files, and {n['sets']} 2 x 2 sets with {n['set_intervals']} "
               f"interaction intervals in the factorial files; replicates dropped as non-finite: "
               f"{len(short)} of {n['intervals'] + n['set_intervals']} intervals" +
               (" (" + "; ".join(short) + ")" if short else "") + ".")
    _, _, bst = blocks_table()
    out.append(
        f"- Sampling error of the estimates ({bst['blocks']} blocks of {bst['per_block']} "
        f"calibration episodes): SE of σ̂ "
        f"{bst['sigma']['se']:.4f} ({100 * bst['sigma']['se'] / SIGMA:.1f}% of {SIGMA:.4f}), SE "
        f"of k̂ {bst['k']['se']:.4f} ({100 * bst['k']['se'] / K:.1f}% of {K:.4f}); the perturbations "
        f"σ×0.8 and σ×1.25 are {bst['sigma']['z_lo']:.0f} and {bst['sigma']['z_hi']:.0f} SE, k×0.5 "
        f"and k×2 are {bst['k']['z_lo']:.0f} and {bst['k']['z_hi']:.0f} SE.")
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
    t_cmp, _ = comparison_table(RES / "baseline" / "comparison.csv", kept=True)
    t_cmp_s, _ = comparison_table(RES / "baseline" / "comparison.csv", as_only=True, kept=True)
    t_stress, _ = stress_table()
    t_scmp, _ = comparison_table(RES / "stress" / "stress_comparison.csv", ("nu", "n_br"))
    t_scmp_s, _ = comparison_table(RES / "stress" / "stress_comparison.csv", ("nu", "n_br"),
                                   as_only=True)
    reps = pd.read_csv(RES / "baseline" / "comparison.csv").replicates.iloc[0]
    t_dec_full, t_dec2_full, _ = decomposition_table(full=True)
    t_dec, t_dec2, dec = decomposition_table(full=False)
    dec_reps = next(iter(dec.values()))["reps"]
    t_blk, t_blk_sum, _ = blocks_table()
    t_sens_full = sensitivity_table(full=True)
    t_sens = sensitivity_table(full=False)
    n_sens = run_field("sensitivity/uninformed_k1_s1", "episodes")
    t_mc, worst = mc_table()
    mc_reps = pd.read_csv(RES / "mc_check" / "baseline_c1" / "comparison.csv").replicates.iloc[0]
    verdict = ("within" if worst <= 0.05 else "above")

    est = (f"**A&S estimates** (calibration run, {n_cal} episodes, seed {seeds['calibration']}): "
           f"σ = {SIGMA:.4f} ticks/√s (10 s returns), k = {K:.4f} per tick, "
           f"A = {float(CAL['A']):.4f} per second per side, R² of the log-linear fit "
           f"{float(CAL['k_fit_r2']):.3f} over {n_used} points. The points are cumulative counts, so each "
           "contains the next and they are strongly dependent: R² overstates how well one "
           "exponential describes the data, and the fit misses the rate at the touch by about a "
           "factor of two (Table 2a), partly by construction, since every market order reaches "
           "at least half a tick. No interval is given for k or σ here; Table 8 gives their "
           "sampling spread across blocks of episodes.")
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
        "B; the last column is the number of replicates with a finite value).", t_cmp,
        f"**Table 5. Stress grid** ({n_stress} episodes per strategy and cell, seed "
        f"{seeds['stress']}; cell (0, -) has no informed flow).", t_stress,
        "**Table 6. Paired comparisons across the stress grid** (same bootstrap).", t_scmp,
        "**Table 7a. Inventory skew and time-varying spread term: 2 x 2 design, both orders** "
        "(simulated; same runs and bootstrap as Tables 4 and 6). Four arms at the same γ: A&S "
        "(skew and time-varying spread), SkewConst (A&S's skew, Symmetric's constant spread), "
        "SymTV (A&S's time-varying spread, no skew) and Symmetric (neither). Spread first adds the "
        "time-varying spread without the skew (SymTV vs Symmetric), then the skew (A&S vs SymTV); "
        "skew first adds the skew without the time-varying spread (SkewConst vs Symmetric), then "
        "the spread term (A&S vs SkewConst). The spread-first order was pre-specified; the "
        "SkewConst arm and the skew-first order were added after an independent review (post "
        "hoc). Within each order the two SD ratios multiply to the A&S vs Symmetric ratio; that "
        "is an algebraic identity (a/c = a/b × b/c), checked by make_tables.py only to confirm "
        "that the pairs use the same SDs, not evidence about the decomposition.", t_dec_full,
        "**Table 7b. Skew shares and interaction** (same cells). Skew share of ln(SD PnL ratio "
        "A&S/Symmetric): spread first ln(A&S/SymTV) / ln(A&S/Symmetric), skew first "
        "ln(SkewConst/Symmetric) / ln(A&S/Symmetric), and their average (the two-way Shapley "
        "share); point estimates, no interval. A share depends on the order whenever the "
        f"interaction is not 1. Interaction: paired percentile bootstrap ({dec_reps} replicates) "
        "with the same resampled episodes for all four arms; 1 (0 for the mean) means the spread "
        "term has the same effect with and without the skew. Where SymTV and Symmetric quote "
        "identically (the time variation never crosses a tick), the spread-first share and the "
        "average are shown as n/a.", t_dec2_full,
        "**Table 8. Sampling spread of the A&S inputs** (simulated; the calibration run of "
        "Table 2a cut into consecutive blocks; each block's k fit uses the δ points of the full "
        "fit). Blocks are independent episodes, so SD/√(blocks) is a standard error of the "
        "full-run estimate. It measures sampling error only, not the misfit of the exponential "
        "at the touch.", t_blk, t_blk_sum,
        f"**Table 9. Sensitivity to the estimated k and σ** (simulated; {n_sens} episodes per "
        f"cell and setting, the first {n_sens} episodes of the baseline (seed "
        f"{seeds['baseline']}) and of the stress run (seed {seeds['stress']}), so every setting "
        f"sees the same episodes). The as, symtv and sym arms get k and σ scaled as shown; γ "
        f"stays at the value chosen from the unperturbed σ̂, so a perturbation moves both the "
        f"skew and the spread. Intervals: paired percentile bootstrap ({reps} replicates).",
        t_sens_full,
        f"**Table 10. Monte Carlo error of the bootstrap endpoints** (simulated). Three headline "
        f"intervals recomputed with {mc_reps} replicates on the same episodes (separate runs of "
        f"the two arms with the same seeds; their summaries are checked to equal the main runs'). "
        f"Pre-specified rule: if an endpoint moves by more than 5% of the width, every comparison "
        f"is rerun with 10,000 replicates. Largest move: {100 * worst:.1f}% of the width, "
        f"{verdict} the 5% threshold. Percentile endpoints are type-7 quantiles of the sorted "
        f"replicates.", t_mc,
    ]
    body = "\n\n".join(parts) + "\n"
    # The README shows A&S comparisons only; tables.md also has Symmetric against NaiveTouch.
    swap = {id(t_cmp): t_cmp_s, id(t_scmp): t_scmp_s, id(t_dec_full): t_dec,
            id(t_dec2_full): t_dec2, id(t_sens_full): t_sens}
    short = "\n\n".join(swap.get(id(p), p) for p in parts)
    short += ("\n\nSymmetric against NaiveTouch comparisons, and Tables 7a and 7b for the other "
              "stress cells, are in `results/backtest/tables.md`."
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
