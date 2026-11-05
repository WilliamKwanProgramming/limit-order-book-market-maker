"""Type stubs for lobsim.paper."""

from typing import Literal, TypedDict, final

import numpy as np
import numpy.typing as npt

__all__ = ["STRATEGIES", "SimResults", "Summary", "simulate", "summarize"]

STRATEGIES: tuple[str, ...]

@final
class SimResults:
    @property
    def profit(self) -> npt.NDArray[np.float64]: ...
    @property
    def final_q(self) -> npt.NDArray[np.int64]: ...
    @property
    def mean_spread(self) -> npt.NDArray[np.float64]: ...
    @property
    def cash(self) -> npt.NDArray[np.float64]: ...
    @property
    def final_mid(self) -> npt.NDArray[np.float64]: ...
    @property
    def bid_fills(self) -> npt.NDArray[np.int64]: ...
    @property
    def ask_fills(self) -> npt.NDArray[np.int64]: ...
    @property
    def gamma(self) -> float: ...
    @property
    def strategy(self) -> str: ...
    @property
    def seed(self) -> int: ...
    def __len__(self) -> int: ...

class Summary(TypedDict):
    n: int
    mean_spread: float
    mean_profit: float
    sd_profit: float
    mean_final_q: float
    sd_final_q: float

def simulate(
    gamma: float,
    sims: int,
    seed: int,
    strategy: Literal["inventory", "symmetric", "symmetric_tv"] = "inventory",
    *,
    s0: float = 100.0,
    sigma: float = 2.0,
    horizon: float = 1.0,
    A: float = 140.0,
    k: float = 1.5,
    steps: int = 200,
) -> SimResults: ...
def summarize(results: SimResults, first: int = 0, count: int | None = None) -> Summary: ...
