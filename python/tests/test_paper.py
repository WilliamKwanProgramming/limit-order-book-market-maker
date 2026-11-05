"""lobsim.paper reproduces the C++ replication's published 1000-simulation run (seed 1)."""

import csv
import math

import numpy as np
import pytest

from lobsim import paper

SUMMARY_FIELDS = ("n", "mean_spread", "mean_profit", "sd_profit", "mean_final_q", "sd_final_q")


def read_csv(path):
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


@pytest.fixture(scope="module")
def sims1000_dir(request):
    from conftest import REPO_ROOT
    d = REPO_ROOT / "results" / "paper" / "sims1000"
    if not (d / "summary.csv").exists():
        pytest.skip(f"{d / 'summary.csv'} not found")
    return d


@pytest.fixture(scope="module")
def seed_from_run_txt(sims1000_dir):
    for line in (sims1000_dir / "run.txt").read_text().splitlines():
        if line.startswith("seed:"):
            return int(line.split(":", 1)[1])
    pytest.fail("no seed line in run.txt")


def test_summary_csv_reproduced(sims1000_dir, seed_from_run_txt):
    rows = read_csv(sims1000_dir / "summary.csv")
    assert rows, "summary.csv is empty"
    assert seed_from_run_txt == 1
    for row in rows:
        gamma = float(row["gamma"])
        n = int(row["n"])
        res = paper.simulate(gamma, n, seed_from_run_txt, row["strategy"])
        got = paper.summarize(res)
        assert got["n"] == n
        for key in SUMMARY_FIELDS[1:]:
            # The file holds 10 significant digits (%.10g).
            assert got[key] == pytest.approx(float(row[key]), rel=1e-9, abs=1e-12), (row, key)


def test_per_simulation_values_reproduced(sims1000_dir, seed_from_run_txt):
    path = sims1000_dir / "sims.csv"
    if not path.exists():
        pytest.skip("sims.csv not found")
    rows = read_csv(path)
    by_cell = {}
    for r in rows:
        by_cell.setdefault((float(r["gamma"]), r["strategy"]), []).append(r)
    assert len(by_cell) == 9
    for (gamma, strategy), cell in by_cell.items():
        res = paper.simulate(gamma, len(cell), seed_from_run_txt, strategy)
        sims = np.array([int(r["sim"]) for r in cell])
        np.testing.assert_array_equal(sims, np.arange(len(cell)))
        for col in ("profit", "cash", "final_mid"):
            np.testing.assert_allclose(getattr(res, col), [float(r[col]) for r in cell],
                                       rtol=1e-9, atol=1e-9)
        for col in ("final_q", "bid_fills", "ask_fills"):
            np.testing.assert_array_equal(getattr(res, col), [int(r[col]) for r in cell])


def test_simulate_result_shapes_and_metadata():
    res = paper.simulate(0.1, 25, 7, "symmetric")
    assert len(res) == 25
    assert res.profit.dtype == np.float64 and res.profit.shape == (25,)
    assert res.final_q.dtype == np.int64
    assert res.mean_spread.dtype == np.float64
    assert (res.gamma, res.strategy, res.seed) == (0.1, "symmetric", 7)
    assert repr(res) == "SimResults(gamma=0.1, strategy='symmetric', seed=7, sims=25)"
    np.testing.assert_array_equal(res.final_q, res.bid_fills - res.ask_fills)
    np.testing.assert_allclose(res.profit, res.cash + res.final_q * res.final_mid, rtol=1e-12)
    with pytest.raises(ValueError):
        res.profit[0] = 0.0  # read-only


def test_simulations_are_prefix_stable_and_seed_dependent():
    a = paper.simulate(0.1, 50, 3)
    b = paper.simulate(0.1, 20, 3)
    np.testing.assert_array_equal(a.profit[:20], b.profit)
    c = paper.simulate(0.1, 20, 4)
    assert not np.array_equal(b.profit, c.profit)


def test_strategies_share_paths_and_differ_in_quotes():
    inv = paper.simulate(0.1, 30, 1, "inventory")
    sym = paper.simulate(0.1, 30, 1, "symmetric")
    tv = paper.simulate(0.1, 30, 1, "symmetric_tv")
    np.testing.assert_array_equal(inv.final_mid, sym.final_mid)  # common random numbers
    np.testing.assert_array_equal(inv.final_mid, tv.final_mid)
    assert np.all(sym.mean_spread == sym.mean_spread[0])
    np.testing.assert_allclose(inv.mean_spread, sym.mean_spread, rtol=1e-12)
    assert not np.array_equal(inv.profit, sym.profit)
    assert not np.array_equal(sym.profit, tv.profit)
    assert paper.STRATEGIES == ("inventory", "symmetric", "symmetric_tv")


def test_model_keywords():
    base = paper.simulate(0.1, 10, 1)
    same = paper.simulate(0.1, 10, 1, s0=100.0, sigma=2.0, horizon=1.0, A=140.0, k=1.5,
                          steps=200)
    np.testing.assert_array_equal(base.profit, same.profit)
    still = paper.simulate(0.1, 10, 1, sigma=0.0)
    np.testing.assert_array_equal(still.final_mid, np.full(10, 100.0))
    shifted = paper.simulate(0.1, 10, 1, s0=50.0)
    np.testing.assert_allclose(shifted.final_mid, base.final_mid - 50.0, rtol=0, atol=1e-9)
    short = paper.simulate(0.1, 10, 1, steps=20)
    assert np.all(short.bid_fills <= 20)


def test_summarize_matches_numpy_and_slices():
    res = paper.simulate(1.0, 200, 9)
    s = paper.summarize(res)
    assert set(s) == set(SUMMARY_FIELDS)
    assert s["n"] == 200
    assert s["mean_profit"] == pytest.approx(res.profit.mean(), rel=1e-12)
    assert s["sd_profit"] == pytest.approx(res.profit.std(ddof=1), rel=1e-12)
    assert s["mean_final_q"] == pytest.approx(res.final_q.mean(), rel=1e-12)
    assert s["sd_final_q"] == pytest.approx(res.final_q.std(ddof=1), rel=1e-12)
    part = paper.summarize(res, first=50, count=100)
    assert part["n"] == 100
    assert part["sd_profit"] == pytest.approx(res.profit[50:150].std(ddof=1), rel=1e-12)
    tail = paper.summarize(res, first=150)
    assert tail["n"] == 50
    assert paper.summarize(res, first=200)["n"] == 0
    with pytest.raises(ValueError):
        paper.summarize(res, first=201)
    with pytest.raises(ValueError):
        paper.summarize(res, first=150, count=51)


def test_zero_simulations():
    res = paper.simulate(0.1, 0, 1)
    assert len(res) == 0 and res.profit.shape == (0,)
    s = paper.summarize(res)
    assert s["n"] == 0 and s["mean_profit"] == 0.0


@pytest.mark.parametrize("kwargs,match", [
    (dict(gamma=0.0), "gamma"),
    (dict(gamma=-1.0), "gamma"),
    (dict(gamma=math.nan), "gamma"),
    (dict(sims=-1), "sims"),
    (dict(strategy="bogus"), "unknown strategy 'bogus'"),
    (dict(steps=0), "steps"),
    (dict(k=0.0), "k must"),
    (dict(sigma=-1.0), "sigma"),
    (dict(horizon=0.0), "horizon"),
    (dict(A=-1.0), "A must"),
    (dict(s0=math.inf), "s0"),
])
def test_invalid_parameters_raise_value_error(kwargs, match):
    args = dict(gamma=0.1, sims=5, seed=1)
    args.update(kwargs)
    with pytest.raises(ValueError, match=match):
        paper.simulate(**args)


def test_wrong_types_raise_type_error():
    with pytest.raises(TypeError):
        paper.simulate(0.1, 5.5, 1)
    with pytest.raises(TypeError):
        paper.simulate(0.1, 5, -1)  # seed is unsigned
    with pytest.raises(TypeError):
        paper.simulate(0.1, 5)  # seed is required
    with pytest.raises(TypeError):
        paper.summarize([1.0, 2.0])
