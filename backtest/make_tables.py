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


KIND_NAME = {"as": "A&S", "sym": "Symmetric", "symtv": "SymTV"}


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
    """Rows ordered NaiveTouch first, then A&S, SymTV, Symmetric for each gamma."""
    order = {"naive": 0, "as": 1, "symtv": 2, "sym": 3}
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
    # Pairs with the symtv arm are in the decomposition table.
    c = c[~c.a.str.startswith("symtv:") & ~c.b.str.startswith("symtv:")]
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


def skew_share(skew_ratio, both_ratio):
    """ln(A&S/SymTV) / ln(A&S/Symmetric); undefined (nan) when A&S and Symmetric have equal SDs."""
    return np.log(skew_ratio) / np.log(both_ratio) if both_ratio != 1 else float("nan")


def fmt_share(x):
    return "undefined (ratio 1)" if np.isnan(x) else f"{x:.2f}"


DECOMP_STATS = [("mean_pnl_diff", "+.1f"), ("sd_pnl_ratio", ".3f"), ("sd_final_inv_ratio", ".3f"),
                ("sharpe_diff", "+.3f")]


def decomposition_rows(c, gamma, lead):
    """A&S/Sym, A&S/SymTV, SymTV/Sym at one gamma, and the product of the two factor ratios.

    The SD ratios multiply exactly (same episodes): A&S/Sym = A&S/SymTV x SymTV/Sym. The skew share
    is ln(A&S/SymTV) / ln(A&S/Sym) for the SD of PnL (a point estimate)."""
    a, s, tv = f"as:{gamma}", f"sym:{gamma}", f"symtv:{gamma}"
    pairs = [(a, s, "A&S vs Symmetric (both effects)"), (a, tv, "A&S vs SymTV (skew)"),
             (tv, s, "SymTV vs Symmetric (spread term)")]
    rows, est = [], {}
    for x, y, what in pairs:
        g = c[(c.a == x) & (c.b == y)].set_index("statistic")
        est[what] = g.estimate
        rows.append(lead + [what] + [ci(g.loc[k, "estimate"], g.loc[k, "ci_lo"], g.loc[k, "ci_hi"], f)
                                     for k, f in DECOMP_STATS] + ["-"])
    both, skew, spread = (est[p[2]] for p in pairs)
    prod = {k: skew[k] * spread[k] for k in ("sd_pnl_ratio", "sd_final_inv_ratio")}
    for k, v in prod.items():  # the identity holds up to the 10 digits written to the CSV
        assert abs(v / both[k] - 1) < 1e-8, (gamma, k, v, both[k])
    share = skew_share(skew["sd_pnl_ratio"], both["sd_pnl_ratio"])
    rows.append(lead + ["product of the two factors", "-", f"{prod['sd_pnl_ratio']:.3f}",
                        f"{prod['sd_final_inv_ratio']:.3f}", "-", fmt_share(share)])
    return rows, share


DECOMP_HEAD = ["Mean PnL A − B (USD)", "SD PnL A / B", "SD q_T A / B", "Mean/SD A − B",
               "Skew share of ln(SD PnL ratio)"]


def decomposition_table(full):
    """Table 7. full=False: the README cells (baseline, stress (0, -) and (0.1, 0.9))."""
    bc = pd.read_csv(RES / "baseline" / "comparison.csv")
    sc = pd.read_csv(RES / "stress" / "stress_comparison.csv")
    rows, shares = [], {}
    gammas = sorted({a[3:] for a in bc.a if a.startswith("as:")}, key=float)
    for g in gammas:
        r, sh = decomposition_rows(bc, g, ["uninformed", f"{skew_c(float(g)):.2g}"])
        rows += r
        shares[("uninformed", g)] = sh
    g = next(a[3:] for a in sc.a if a.startswith("as:"))
    for (nu, nb), cell in sc.groupby(["nu", "n_br"], sort=False):
        name = "stress (0, -)" if nu == 0 else f"stress ({nu:g}, {nb:g})"
        keep = nu == 0 or (np.isclose(nu, 0.1) and np.isclose(nb, 0.9))
        if not (full or keep):
            continue
        r, sh = decomposition_rows(cell, g, [name, f"{skew_c(float(g)):.2g}"])
        rows += r
        shares[name] = sh
    return md(["Run", "c", "A vs B"] + DECOMP_HEAD, rows), shares


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
            "A&S SD PnL (USD)", "SD PnL A&S / Symmetric", "SD PnL A&S / SymTV (skew)",
            "SD PnL SymTV / Symmetric (spread term)"]
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
    """Intervals in any comparison file with fewer kept replicates than drawn."""
    files = sorted(RES.glob("*/comparison.csv")) + sorted(RES.glob("*/stress_comparison.csv")) + \
        sorted(RES.glob("*/*/comparison.csv"))
    total, short = 0, []
    for f in files:
        c = pd.read_csv(f)
        total += len(c)
        d = c[c.replicates_kept < c.replicates]
        short += [f"{f.relative_to(ROOT)}: {r.a} vs {r.b} {r.statistic} ({r.replicates_kept} of "
                  f"{r.replicates})" for _, r in d.iterrows()]
    return total, short


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
    # Decomposition (Table 7): the rows named in the pre-specified reading.
    gm = a_s.strategy.iloc[2][3:]
    for name, c, g, cell in [("uninformed, c=0.1", bc, gm, {}),
                             ("uninformed, c=1", bc, g_hi[3:], {}),
                             ("stress (0, -), c=0.1", sc, a_st[3:], {"nu": 0}),
                             ("stress (0.1, 0.9), c=0.1", sc, a_st[3:], {"nu": 0.1, "n_br": 0.9})]:
        both = fmt_iv(c, f"as:{g}", f"sym:{g}", "sd_pnl_ratio", ".3f", **cell)
        skew = fmt_iv(c, f"as:{g}", f"symtv:{g}", "sd_pnl_ratio", ".3f", **cell)
        spread = fmt_iv(c, f"symtv:{g}", f"sym:{g}", "sd_pnl_ratio", ".3f", **cell)
        d = c
        for k, v in cell.items():
            d = d[np.isclose(d[k], v)]
        e = d[d.statistic == "sd_pnl_ratio"].set_index(["a", "b"]).estimate
        share = skew_share(e[(f"as:{g}", f"symtv:{g}")], e[(f"as:{g}", f"sym:{g}")])
        inv_skew = fmt_iv(c, f"as:{g}", f"symtv:{g}", "sd_final_inv_ratio", ".3f", **cell)
        inv_spread = fmt_iv(c, f"symtv:{g}", f"sym:{g}", "sd_final_inv_ratio", ".3f", **cell)
        out.append(
            f"- Decomposition, {name}: SD PnL ratio A&S/Symmetric {both} = A&S/SymTV (skew) "
            f"{skew} × SymTV/Symmetric (spread term) {spread}; skew share {fmt_share(share)}. SD q_T "
            f"ratio A&S/SymTV {inv_skew}, SymTV/Symmetric {inv_spread}.")
    out += sensitivity_bullets()
    total, short = dropped_replicates()
    out.append(f"- Bootstrap replicates dropped as non-finite: {len(short)} of {total} intervals "
               "in the comparison files" + (": " + "; ".join(short) if short else "") + ".")
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
    t_dec_full, _ = decomposition_table(full=True)
    t_dec, _ = decomposition_table(full=False)
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
        "B; the last column is the number of replicates with a finite value).", t_cmp,
        f"**Table 5. Stress grid** ({n_stress} episodes per strategy and cell, seed "
        f"{seeds['stress']}; cell (0, -) has no informed flow).", t_stress,
        "**Table 6. Paired comparisons across the stress grid** (same bootstrap).", t_scmp,
        "**Table 7. Inventory skew and spread term, separated** (simulated; same runs and bootstrap "
        "as Tables 4 and 6). SymTV quotes the mid ∓ half of A&S's spread at the current time "
        "with no skew, so A&S vs SymTV isolates the inventory skew and SymTV vs Symmetric the "
        "time-varying spread term. The SD ratios multiply exactly: the product row equals the "
        "A&S vs Symmetric row. Skew share = ln(SD PnL ratio A&S/SymTV) / ln(SD PnL ratio "
        "A&S/Symmetric), a point estimate with no interval.", t_dec_full,
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
            id(t_sens_full): t_sens}
    short = "\n\n".join(swap.get(id(p), p) for p in parts)
    short += ("\n\nSymmetric against NaiveTouch comparisons, and Table 7 for the other stress "
              "cells, are in `results/backtest/tables.md`."
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
