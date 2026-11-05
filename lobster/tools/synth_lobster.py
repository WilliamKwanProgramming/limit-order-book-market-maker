#!/usr/bin/env python3
"""Synthetic LOBSTER-layout message and order-book files from a toy order flow.

Writes PREFIX_message_<N>.csv and PREFIX_orderbook_<N>.csv in the LOBSTER layout (no header):

    message row:    time,type,order_id,size,price,direction
    order-book row: ask_px_1,ask_sz_1,bid_px_1,bid_sz_1, ... for levels 1..N
                    (empty levels: ask 9999999999 / bid -9999999999, size 0)

Each order-book row is the state of this script's own book after the message on the same line.
The book and the flow are written in plain standard-library Python and share no code or design
with the C++ engine in core/, so replaying these files with lobster_replay and comparing against
the order-book rows is a cross-implementation check. Both were written for this repository; this
is not a model of any real market and not a substitute for real LOBSTER data.

BOOK. One dict per side, price -> list of [order_id, size] in arrival order, plus an id -> (side,
price) map. Best prices are found with max()/min() over the dict keys; linear scans everywhere.

FLOW MODEL (toy). Prices are in LOBSTER units (USD x 10000) on a 100-unit (one cent) grid,
starting near 1,000,000 ($100). Each step draws one event; its messages share one timestamp.
The probability of a new order falls as the book grows (target about 80 resting orders), so the
book size is mean-reverting. Events and their relative weights after the new-order draw:

    new limit order (type 1)  side 50/50. With probability 0.2 (if the spread is wider than one
                              tick) a price strictly inside the spread; otherwise at an
                              exponentially distributed number of ticks behind the own best price
                              (mean about 3.5, at most 40), or one tick better than it with
                              probability 0.1, never crossing the opposite best. Sizes from a
                              fixed list.
    deletion (type 3)         weight 0.55: a uniformly chosen resting order, full remaining size.
    partial cancel (type 2)   weight 0.12: a resting order of size >= 2 loses 1..size-1.
    execution (type 4)        weight 0.13: the front order at the best price of a random side
                              executes 1..size (full with probability 0.5).
    sweep (type 4 rows)       weight 0.08: a marketable order with a limit 0-3 ticks through the
                              opposite best walks the book, one type-4 row per resting order hit,
                              all with the same timestamp. If it exhausts the liquidity up to its
                              limit, the remainder rests as a type-1 order at the limit price
                              (same timestamp) with probability 0.5.
    clear side (type 4 rows)  weight 0.005: a sweep that takes the whole side (empty side).
    hidden execution (type 5) weight 0.06: fresh order id, price near the touch; book unchanged.
    cross trade (type 6)      weight 0.015: order id 0, price near the mid; book unchanged.
    halt (type 7)             weight 0.01: order id 0, size 0, price -1, 0 or 1, direction 0;
                              book unchanged.

Order ids of new orders are unique and increasing with random gaps. Timestamps are
non-decreasing: the gap between events is 0 with probability 0.1 and otherwise 1 ns plus an
exponential with mean 3 ms, written as seconds after midnight with 9 decimals; in 30% of rows
trailing zeros of the fraction are dropped. The book is asserted never to be crossed or locked.

--cut K simulates K extra messages first and does not write them (nor their book rows); the book
keeps the orders they created. This emulates a file that starts after the open.

Uses random.Random(seed) only; output is a deterministic function of the arguments and the
Python version's random module.
"""

import argparse
import random
import sys

TICK = 100
START_PRICE = 1_000_000
EMPTY_ASK = 9999999999
EMPTY_BID = -9999999999
BUY, SELL = 1, -1
SIZES = (1, 5, 10, 25, 50, 100, 100, 100, 100, 200, 200, 300, 500, 1000)
TARGET_ORDERS = 80


class Book:
    def __init__(self):
        self.levels = {BUY: {}, SELL: {}}  # price -> [[order_id, size], ...]
        self.where = {}                    # order_id -> (direction, price)
        self.ids = []                      # resting ids, for uniform random choice
        self.slot = {}                     # order_id -> index in self.ids

    def best(self, d):
        prices = self.levels[d]
        if not prices:
            return None
        return max(prices) if d == BUY else min(prices)

    def size_of(self, oid):
        d, price = self.where[oid]
        for entry in self.levels[d][price]:
            if entry[0] == oid:
                return entry[1]
        raise AssertionError("order %d not in its level" % oid)

    def add(self, oid, d, price, size):
        assert oid not in self.where, "duplicate id"
        assert size > 0 and price > 0
        self.levels[d].setdefault(price, []).append([oid, size])
        self.where[oid] = (d, price)
        self.slot[oid] = len(self.ids)
        self.ids.append(oid)

    def take(self, oid, size):
        """Remove `size` from an order (cancel, deletion or execution)."""
        d, price = self.where[oid]
        queue = self.levels[d][price]
        for i, entry in enumerate(queue):
            if entry[0] == oid:
                assert 0 < size <= entry[1]
                entry[1] -= size
                if entry[1] == 0:
                    del queue[i]
                    if not queue:
                        del self.levels[d][price]
                    del self.where[oid]
                    k = self.slot.pop(oid)
                    last = self.ids.pop()
                    if last != oid:
                        self.ids[k] = last
                        self.slot[last] = k
                return
        raise AssertionError("order %d not found" % oid)

    def top(self, d, n):
        prices = sorted(self.levels[d], reverse=(d == BUY))[:n]
        return [(p, sum(e[1] for e in self.levels[d][p])) for p in prices]

    def orders_from_best(self, d):
        """Resting orders of side d, best price first, oldest first within a price."""
        for p in sorted(self.levels[d], reverse=(d == BUY)):
            for oid, size in self.levels[d][p]:
                yield oid, p, size


class Flow:
    def __init__(self, seed):
        self.rng = random.Random(seed)
        self.book = Book()
        self.t = 34200 * 10**9 + self.rng.randint(0, 10**9)
        self.next_id = 10_000_000 + self.rng.randint(0, 10**6)
        self.ref = START_PRICE
        self.counts = {"inside_spread_adds": 0, "sweeps": 0, "multi_row_sweeps": 0,
                       "sweep_remainders": 0, "clear_side": 0}

    def new_id(self):
        self.next_id += 1
        if self.rng.random() < 0.25:
            self.next_id += self.rng.randint(1, 40)
        return self.next_id

    def size(self):
        return self.rng.choice(SIZES)

    # Each event returns a list of messages [type, id, size, price, direction]; they are applied
    # to the book one by one, in order, by the caller.

    def ev_new(self):
        rng, book = self.rng, self.book
        d = rng.choice((BUY, SELL))
        bb, ba = book.best(BUY), book.best(SELL)
        if bb is not None and ba is not None and ba - bb > TICK and rng.random() < 0.2:
            price = bb + TICK * rng.randint(1, (ba - bb) // TICK - 1)
            self.counts["inside_spread_adds"] += 1
        else:
            own, opp = book.best(d), book.best(-d)
            if own is None:
                base = opp - d * TICK if opp is not None else self.ref
            else:
                base = own
            if rng.random() < 0.1:
                price = base + d * TICK
            else:
                price = base - d * TICK * min(int(rng.expovariate(0.25)), 40)
            if d == BUY and ba is not None and price >= ba:
                price = ba - TICK
            if d == SELL and bb is not None and price <= bb:
                price = bb + TICK
            price = max(price, TICK)
        return [[1, self.new_id(), self.size(), price, d]]

    def random_order(self):
        oid = self.rng.choice(self.book.ids)
        d, price = self.book.where[oid]
        return oid, d, price, self.book.size_of(oid)

    def ev_delete(self):
        oid, d, price, size = self.random_order()
        return [[3, oid, size, price, d]]

    def ev_partial(self):
        for _ in range(5):
            oid, d, price, size = self.random_order()
            if size >= 2:
                return [[2, oid, self.rng.randint(1, size - 1), price, d]]
        return self.ev_delete()

    def ev_execute(self):
        sides = [d for d in (BUY, SELL) if self.book.levels[d]]
        d = self.rng.choice(sides)
        p = self.book.best(d)
        oid, size = self.book.levels[d][p][0]
        q = size if self.rng.random() < 0.5 else self.rng.randint(1, size)
        return [[4, oid, q, p, d]]

    def ev_sweep(self, clear=False):
        rng, book = self.rng, self.book
        sides = [d for d in (BUY, SELL) if book.levels[d]]
        rest = rng.choice(sides)  # side being hit
        best = book.best(rest)
        if clear:
            limit = None
        else:
            limit = best - rest * TICK * rng.randint(0, 3)  # through the resting side's best
        within = [(oid, p, s) for oid, p, s in book.orders_from_best(rest)
                  if limit is None or (p >= limit if rest == BUY else p <= limit)]
        avail = sum(s for _, _, s in within)
        if clear:
            qty = avail
        else:
            qty = rng.randint(1, max(1, int(avail * rng.uniform(0.3, 1.3))))
        msgs = []
        left = qty
        for oid, p, s in within:
            if left == 0:
                break
            take = min(left, s)
            msgs.append([4, oid, take, p, rest])
            left -= take
        self.counts["sweeps"] += 1
        if len(msgs) > 1:
            self.counts["multi_row_sweeps"] += 1
        if clear:
            self.counts["clear_side"] += 1
        elif left > 0 and rng.random() < 0.5:
            msgs.append([1, self.new_id(), left, limit, -rest])
            self.counts["sweep_remainders"] += 1
        return msgs

    def near_touch(self, d):
        best = self.book.best(d)
        base = best if best is not None else self.ref
        return base - d * TICK * self.rng.randint(-1, 2)

    def ev_hidden(self):
        d = self.rng.choice((BUY, SELL))
        return [[5, self.new_id(), self.size(), max(TICK, self.near_touch(d)), d]]

    def ev_cross(self):
        return [[6, 0, self.rng.randint(1, 50) * 100, self.ref, self.rng.choice((BUY, SELL))]]

    def ev_halt(self):
        return [[7, 0, 0, self.rng.choice((-1, 0, 1)), 0]]

    def step(self):
        """Draw one event; returns (time, messages)."""
        rng, book = self.rng, self.book
        n = len(book.ids)
        p_new = min(0.75, max(0.25, 0.5 + 0.2 * (TARGET_ORDERS - n) / TARGET_ORDERS))
        if n == 0 or rng.random() < p_new:
            msgs = self.ev_new()
        else:
            kind = rng.choices(
                ("delete", "partial", "execute", "sweep", "clear", "hidden", "cross", "halt"),
                weights=(0.55, 0.12, 0.13, 0.08, 0.005, 0.06, 0.015, 0.01))[0]
            if kind == "delete":
                msgs = self.ev_delete()
            elif kind == "partial":
                msgs = self.ev_partial()
            elif kind == "execute":
                msgs = self.ev_execute()
            elif kind == "sweep":
                msgs = self.ev_sweep()
            elif kind == "clear":
                msgs = self.ev_sweep(clear=True)
            elif kind == "hidden":
                msgs = self.ev_hidden()
            elif kind == "cross":
                msgs = self.ev_cross()
            else:
                msgs = self.ev_halt()
        if rng.random() >= 0.1:
            self.t += 1 + int(rng.expovariate(1.0 / 3e6))
        return self.t, msgs

    def apply(self, m):
        mtype, oid, size, price, d = m
        if mtype == 1:
            self.book.add(oid, d, price, size)
        elif mtype in (2, 4):
            assert self.book.where[oid] == (d, price)
            assert size <= self.book.size_of(oid)
            if mtype == 2:
                assert size < self.book.size_of(oid)
            self.book.take(oid, size)
        elif mtype == 3:
            assert self.book.where[oid] == (d, price)
            assert size == self.book.size_of(oid)
            self.book.take(oid, size)
        bb, ba = self.book.best(BUY), self.book.best(SELL)
        assert bb is None or ba is None or bb < ba, "book crossed or locked"
        if bb is not None and ba is not None:
            self.ref = (bb + ba) // 2 // TICK * TICK


def format_time(t_ns, rng):
    s, f = divmod(t_ns, 10**9)
    text = "%d.%09d" % (s, f)
    if rng.random() < 0.3:
        text = text.rstrip("0").rstrip(".")
    return text


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--messages", type=int, required=True, help="message rows to write (M)")
    ap.add_argument("--levels", type=int, required=True, help="order-book levels per side (N)")
    ap.add_argument("--seed", type=int, required=True)
    ap.add_argument("--out", required=True, help="output prefix PREFIX")
    ap.add_argument("--cut", type=int, default=0,
                    help="simulate K messages first and do not write them (default 0)")
    args = ap.parse_args(argv)
    if args.messages < 1 or args.levels < 1 or args.cut < 0:
        ap.error("--messages and --levels must be >= 1, --cut >= 0")

    flow = Flow(args.seed)
    fmt_rng = random.Random(args.seed ^ 0x5EED)  # time formatting only
    n_levels = args.levels
    msg_path = "%s_message_%d.csv" % (args.out, n_levels)
    book_path = "%s_orderbook_%d.csv" % (args.out, n_levels)
    by_type = [0] * 8
    info = {"rows_with_an_empty_side": 0, "rows_with_fewer_than_n_levels": 0,
            "rows_sharing_previous_timestamp": 0, "max_resting_orders": 0}
    total = args.cut + args.messages
    done = 0
    last_t = None
    with open(msg_path, "w", newline="\n") as mf, open(book_path, "w", newline="\n") as bf:
        while done < total:
            t, msgs = flow.step()
            for m in msgs:
                if done == total:
                    break
                flow.apply(m)
                done += 1
                if done <= args.cut:
                    continue
                mtype, oid, size, price, d = m
                by_type[mtype] += 1
                mf.write("%s,%d,%d,%d,%d,%d\n" % (format_time(t, fmt_rng), mtype, oid, size,
                                                  price, d))
                asks = flow.book.top(SELL, n_levels)
                bids = flow.book.top(BUY, n_levels)
                fields = []
                for i in range(n_levels):
                    ap_, as_ = asks[i] if i < len(asks) else (EMPTY_ASK, 0)
                    bp_, bs_ = bids[i] if i < len(bids) else (EMPTY_BID, 0)
                    fields += (ap_, as_, bp_, bs_)
                bf.write(",".join(map(str, fields)) + "\n")
                if not asks or not bids:
                    info["rows_with_an_empty_side"] += 1
                if len(asks) < n_levels or len(bids) < n_levels:
                    info["rows_with_fewer_than_n_levels"] += 1
                if last_t == t:
                    info["rows_sharing_previous_timestamp"] += 1
                last_t = t
                info["max_resting_orders"] = max(info["max_resting_orders"], len(flow.book.ids))

    print("message_file=%s" % msg_path)
    print("orderbook_file=%s" % book_path)
    print("seed=%d messages=%d levels=%d cut=%d" % (args.seed, args.messages, n_levels, args.cut))
    print("by_type=" + ",".join("%d:%d" % (k, by_type[k]) for k in range(1, 8)))
    for k, v in sorted(info.items()):
        print("%s=%d" % (k, v))
    for k, v in sorted(flow.counts.items()):
        print("events_%s=%d" % (k, v))
    print("resting_orders_at_end=%d" % len(flow.book.ids))
    return 0


if __name__ == "__main__":
    sys.exit(main())
