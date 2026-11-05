"""lobsim: Python bindings for the C++17 limit order book engine and the Avellaneda-Stoikov
(2008) simulation of this repository.

The compiled part is the extension module ``lobsim._lobsim``; this package re-exports it.

    OrderBook      price-time priority book (matching mode and replay mode)
    Side, Status   enums matching lob::Side (Bid = 1, Ask = -1) and lob::Status
    Fill, Level, OrderInfo, SubmitResult   read-only value classes
    replay_lobster bulk replay of LOBSTER-style message arrays without the GIL
    paper          lobsim.paper.simulate / summarize (Avellaneda-Stoikov replication)
"""

from ._lobsim import (
    Fill,
    Level,
    OrderBook,
    OrderInfo,
    ReplayResult,
    Side,
    Status,
    SubmitResult,
    replay_lobster,
)
from . import paper

__all__ = [
    "Fill",
    "Level",
    "OrderBook",
    "OrderInfo",
    "ReplayResult",
    "Side",
    "Status",
    "SubmitResult",
    "paper",
    "replay_lobster",
]
