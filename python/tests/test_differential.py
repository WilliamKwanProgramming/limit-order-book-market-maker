"""Randomized differential test: lobsim.OrderBook against the pure-Python RefBook.

Each run applies LOBSIM_DIFF_OPS (default 100,000) random operations, including invalid ones
(id 0, unknown and duplicate ids, non-positive prices and sizes), to both books and compares,
after every operation: the status, filled and resting sizes and every fill; best bid and ask;
order count, level counts and side totals; the top 5 levels of both sides; and find, front and
queue_ahead for a random id. Every 2,000 operations and at the end it compares the full depth,
find and queue_ahead of every resting order, and check_invariants().
"""

import os
import random

import pytest

from lobsim import OrderBook, Side
from reference_book import ASK, BID, RefBook

N_OPS = int(os.environ.get("LOBSIM_DIFF_OPS", "100000"))
SEEDS = [1, 2, 3]
SIDE = {BID: Side.Bid, ASK: Side.Ask}


def fills_of(r):
    return [(f.resting_id, f.aggressor_id, int(f.resting_side), f.price, f.qty, f.ts,
             f.resting_left) for f in r.fills]


def level_tuple(lv):
    return None if lv is None else (lv.price, lv.qty, lv.count)


def info_tuple(o):
    return None if o is None else (o.id, int(o.side), o.price, o.qty, o.ts)


def compare_light(book, ref, rng, ids):
    for s in (BID, ASK):
        side = SIDE[s]
        assert level_tuple(book.best(side)) == ref.best(s)
        assert book.level_count(side) == ref.level_count(s)
        assert book.total_qty(side) == ref.total_qty(s)
        assert [tuple(r) for r in book.depth(side, 5).tolist()] == ref.depth(s, 5)
        assert info_tuple(book.front(side)) == ref.front(s)
    assert book.order_count() == ref.order_count() == len(book)
    assert book.spread() == ref.spread()
    assert book.mid() == ref.mid()
    oid = rng.choice(ids) if ids else 1
    assert info_tuple(book.find(oid)) == ref.find(oid)
    assert book.queue_ahead(oid) == ref.queue_ahead(oid)


def compare_full(book, ref, uncrossed):
    for s in (BID, ASK):
        n = ref.level_count(s) + 3
        assert [tuple(r) for r in book.depth(SIDE[s], n).tolist()] == ref.depth(s, n)
    for oid in list(ref.where):
        assert info_tuple(book.find(oid)) == ref.find(oid)
        assert book.queue_ahead(oid) == ref.queue_ahead(oid)
    ok, why = book.check_invariants(uncrossed)
    assert ok, why


class OpGen:
    """Random operations over a small id space so that ids are reused, duplicated and unknown."""

    def __init__(self, seed, cross_p):
        self.rng = random.Random(seed)
        self.cross_p = cross_p
        self.next_id = 1
        self.used: list[int] = []
        self.ts = 0

    def new_id(self):
        r = self.rng.random()
        if r < 0.01:
            return 0
        if r < 0.04 and self.used:
            return self.rng.choice(self.used[-50:])  # often still resting -> DuplicateId
        oid = self.next_id
        self.next_id += 1
        self.used.append(oid)
        if len(self.used) > 4000:
            self.used = self.used[-2000:]
        return oid

    def old_id(self, live):
        r = self.rng.random()
        if r < 0.01:
            return 0
        if r < 0.06 or not live:
            return self.rng.randint(1, self.next_id + 10)  # mostly gone or never used
        return self.rng.choice(live)

    def price(self, side):
        """Mostly passive around a fixed mid of 1000; a fraction cross_p is at or through the
        mid (so it trades, or in replay mode may lock or cross the book)."""
        r = self.rng.random()
        if r < 0.005:
            return self.rng.choice([0, -1, -100])
        if r < 0.005 + self.cross_p:
            return 1000 + side * self.rng.randint(0, 10)
        return 1000 - side * self.rng.randint(1, 20)

    def qty(self):
        r = self.rng.random()
        if r < 0.005:
            return self.rng.choice([0, -1])
        return self.rng.randint(1, 100) if r < 0.9 else self.rng.randint(100, 1000)

    def side(self):
        return BID if self.rng.random() < 0.5 else ASK

    def tick(self):
        self.ts += self.rng.randint(0, 3)
        return self.ts


def apply(book, ref, op, args):
    if op in ("submit_limit", "submit_market", "replace", "execute_resting"):
        if op in ("submit_limit", "submit_market"):
            py_args = list(args)
            py_args[1] = SIDE[args[1]]
        else:
            py_args = args
        r = getattr(book, op)(*py_args)
        expected = getattr(ref, op)(*args)
        got = (r.status.name, r.filled, r.resting, fills_of(r))
        assert got == expected, (op, args)
    else:
        py_args = list(args)
        if op == "add_resting":
            py_args[1] = SIDE[args[1]]
        st = getattr(book, op)(*py_args)
        assert st.name == getattr(ref, op)(*args), (op, args)


def run(seed, mode):
    g = OpGen(seed, cross_p=0.15 if mode == "matching" else 0.03)
    rng = g.rng
    book, ref = OrderBook(), RefBook()
    live_cache: list[int] = []
    for step in range(N_OPS):
        if step % 10 == 0:
            live_cache = list(ref.where)
        r = rng.random()
        if mode == "matching":
            if r < 0.40:
                sd = g.side()
                op, args = "submit_limit", (g.new_id(), sd, g.price(sd), g.qty(), g.tick())
            elif r < 0.47:
                op, args = "submit_market", (g.new_id(), g.side(), g.qty(), g.tick())
            elif r < 0.67:
                op, args = "cancel", (g.old_id(live_cache),)
            elif r < 0.77:
                op, args = "reduce", (g.old_id(live_cache), g.qty())
            else:
                oid = g.old_id(live_cache)
                cur = ref.find(oid)
                if cur is not None and rng.random() < 0.5:
                    # Same price, often a smaller size: exercises the keep-priority branch.
                    op, args = "replace", (oid, cur[2], rng.randint(1, cur[3] + 2), g.tick())
                else:
                    sd = cur[1] if cur is not None else g.side()
                    op, args = "replace", (oid, g.price(sd), g.qty(), g.tick())
        else:  # replay mode, with a few matching calls mixed in (allowed on one book)
            if r < 0.40:
                sd = g.side()
                op, args = "add_resting", (g.new_id(), sd, g.price(sd), g.qty(), g.tick())
            elif r < 0.70:
                op, args = "cancel", (g.old_id(live_cache),)
            elif r < 0.80:
                op, args = "reduce", (g.old_id(live_cache), g.qty())
            elif r < 0.95:
                op, args = "execute_resting", (g.old_id(live_cache), g.qty(), g.tick())
            elif r < 0.98:
                sd = g.side()
                op, args = "submit_limit", (g.new_id(), sd, g.price(sd), g.qty(), g.tick())
            else:
                op, args = "submit_market", (g.new_id(), g.side(), g.qty(), g.tick())
        apply(book, ref, op, args)
        compare_light(book, ref, rng, live_cache)
        if step % 2000 == 0:
            compare_full(book, ref, uncrossed=(mode == "matching"))
    compare_full(book, ref, uncrossed=(mode == "matching"))
    return book


@pytest.mark.parametrize("seed", SEEDS)
@pytest.mark.parametrize("mode", ["matching", "replay"])
def test_differential(mode, seed):
    book = run(seed, mode)
    assert len(book) > 0  # the stream kept a non-trivial book
