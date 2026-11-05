"""Avellaneda and Stoikov (2008) simulation, built on the C++ module paper/ (lob::paper).

    res = simulate(gamma=0.1, sims=1000, seed=1, strategy="inventory")
    summarize(res)   # dict: n, mean_spread, mean_profit, sd_profit, mean_final_q, sd_final_q

Simulation i of a call uses the random path Rng::for_stream(seed, i), exactly as the
paper_replication command-line tool does, so the same seed reproduces results/paper/.
"""

from typing import TypedDict

from ._lobsim import paper as _paper

SimResults = _paper.SimResults
simulate = _paper.simulate
summarize = _paper.summarize
STRATEGIES = _paper.STRATEGIES


class Summary(TypedDict):
    """The dict returned by summarize(); keys match results/paper/*/summary.csv."""

    n: int
    mean_spread: float
    mean_profit: float
    sd_profit: float
    mean_final_q: float
    sd_final_q: float


__all__ = ["STRATEGIES", "SimResults", "Summary", "simulate", "summarize"]
