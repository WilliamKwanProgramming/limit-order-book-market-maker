"""lobsim quickstart: build a book, trade against it, print depth, and run the Avellaneda-Stoikov
(2008) simulation. Writes one figure to the path given on the command line.

    PYTHONPATH=build-python/python python3 python/examples/quickstart.py /tmp/quickstart.png

Uses only lobsim, numpy and matplotlib; runs in a few seconds.
"""

import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402

import lobsim  # noqa: E402
from lobsim import OrderBook, Side  # noqa: E402

BLUE, ORANGE = "#2a78d6", "#eb6834"  # categorical slots 1 and 2


def book_demo():
    book = OrderBook()
    # Prices are integer ticks; here 1 tick = 0.01 USD.
    rng = np.random.default_rng(1)
    oid = 1
    for _ in range(40):
        for side in (Side.Bid, Side.Ask):
            price = 10_000 - int(side) * int(rng.integers(1, 12))
            book.submit_limit(oid, side, price, int(rng.integers(1, 9)) * 100)
            oid += 1
    print(book)
    print("best bid", book.best_bid(), "best ask", book.best_ask(), "spread", book.spread())

    r = book.submit_limit(oid, Side.Bid, 10_003, 1_500, ts=1)  # marketable: sweeps asks to 10003
    print(f"aggressive bid: {r.status.name}, filled {r.filled}, resting {r.resting}, "
          f"{len(r.fills)} fills; first fill {r.fills[0] if r.fills else None}")
    print("ask depth (price, qty, count), best first:\n", book.depth(Side.Ask, 5))
    return book


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    out = sys.argv[1]

    book = book_demo()

    inv = lobsim.paper.simulate(0.1, 1000, seed=1, strategy="inventory")
    sym = lobsim.paper.simulate(0.1, 1000, seed=1, strategy="symmetric")
    for res in (inv, sym):
        s = lobsim.paper.summarize(res)
        print(f"{res.strategy:10s} gamma={res.gamma}: mean profit {s['mean_profit']:.2f}, "
              f"sd {s['sd_profit']:.2f}, sd(final q) {s['sd_final_q']:.2f}")

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(10, 4), constrained_layout=True)
    for side, color, name in ((Side.Bid, BLUE, "bids"), (Side.Ask, ORANGE, "asks")):
        d = book.depth(side, 50)
        ax1.bar(d[:, 0], d[:, 1], width=0.8, color=color, label=name)
    ax1.set_xlabel("price (ticks)")
    ax1.set_ylabel("resting size (shares)")
    ax1.set_title("Book after one aggressive bid")
    ax1.legend(frameon=False)

    bins = np.linspace(min(inv.profit.min(), sym.profit.min()),
                       max(inv.profit.max(), sym.profit.max()), 40)
    ax2.hist(inv.profit, bins=bins, histtype="step", linewidth=2, color=BLUE, label="inventory")
    ax2.hist(sym.profit, bins=bins, histtype="step", linewidth=2, color=ORANGE, label="symmetric")
    ax2.set_xlabel("profit")
    ax2.set_ylabel("simulations")
    ax2.set_title("Avellaneda-Stoikov, gamma = 0.1, 1000 sims, seed 1")
    ax2.legend(frameon=False)
    for ax in (ax1, ax2):
        ax.spines[["top", "right"]].set_visible(False)
        ax.grid(axis="y", color="#e5e5e5", linewidth=0.8)
        ax.set_axisbelow(True)
    fig.savefig(out, dpi=120)
    print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
