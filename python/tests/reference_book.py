"""Independent, deliberately simple pure-Python order book used as the oracle in the differential
tests. It is written from the contract in docs/design.md and core/include/lob/order_book.hpp, not
from the C++ source: dict of price -> deque of [id, qty, ts] per side, best price found by
max()/min() on every call.

Statuses are returned as the strings of lobsim.Status names; fills as tuples
(resting_id, aggressor_id, resting_side, price, qty, ts, resting_left) with sides as +1 / -1.
"""

from __future__ import annotations

from collections import deque

BID, ASK = 1, -1


class RefBook:
    def __init__(self) -> None:
        self.levels: dict[int, dict[int, deque]] = {BID: {}, ASK: {}}
        self.where: dict[int, tuple[int, int]] = {}  # id -> (side, price)

    # ---- helpers -----------------------------------------------------------------------------

    @staticmethod
    def _validate_new(oid: int, price: int, qty: int) -> str:
        if oid == 0:
            return "InvalidId"
        if price <= 0:
            return "InvalidPrice"
        if qty <= 0:
            return "InvalidQty"
        return "Ok"

    def _best_price(self, side: int):
        lv = self.levels[side]
        if not lv:
            return None
        return max(lv) if side == BID else min(lv)

    def _order(self, oid: int):
        side, price = self.where[oid]
        for o in self.levels[side][price]:
            if o[0] == oid:
                return o
        raise AssertionError("index out of sync")

    def _remove(self, oid: int) -> None:
        side, price = self.where.pop(oid)
        q = self.levels[side][price]
        for o in q:
            if o[0] == oid:
                q.remove(o)
                break
        if not q:
            del self.levels[side][price]

    def _rest(self, oid: int, side: int, price: int, qty: int, ts: int) -> None:
        self.levels[side].setdefault(price, deque()).append([oid, qty, ts])
        self.where[oid] = (side, price)

    def _match(self, aggressor: int, side: int, limit, qty: int, ts: int, fills: list) -> int:
        opp = -side
        filled = 0
        while qty > 0:
            best = self._best_price(opp)
            if best is None:
                break
            if limit is not None and (best > limit if side == BID else best < limit):
                break
            q = self.levels[opp][best]
            while qty > 0 and q:
                o = q[0]
                take = min(qty, o[1])
                o[1] -= take
                qty -= take
                filled += take
                fills.append((o[0], aggressor, opp, best, take, ts, o[1]))
                if o[1] == 0:
                    q.popleft()
                    del self.where[o[0]]
            if not q:
                del self.levels[opp][best]
        return filled

    # ---- matching mode -----------------------------------------------------------------------

    def submit_limit(self, oid, side, price, qty, ts):
        fills: list = []
        st = self._validate_new(oid, price, qty)
        if st != "Ok":
            return st, 0, 0, fills
        if oid in self.where:
            return "DuplicateId", 0, 0, fills
        filled = self._match(oid, side, price, qty, ts, fills)
        resting = qty - filled
        if resting > 0:
            self._rest(oid, side, price, resting, ts)
        return "Ok", filled, resting, fills

    def submit_market(self, oid, side, qty, ts):
        fills: list = []
        if oid == 0:
            return "InvalidId", 0, 0, fills
        if qty <= 0:
            return "InvalidQty", 0, 0, fills
        if oid in self.where:
            return "DuplicateId", 0, 0, fills
        return "Ok", self._match(oid, side, None, qty, ts, fills), 0, fills

    def replace(self, oid, new_price, new_qty, ts):
        st = self._validate_new(oid, new_price, new_qty)
        if st != "Ok":
            return st, 0, 0, []
        if oid not in self.where:
            return "UnknownId", 0, 0, []
        side, price = self.where[oid]
        o = self._order(oid)
        if new_price == price and new_qty <= o[1]:
            o[1] = new_qty
            return "Ok", 0, new_qty, []
        self._remove(oid)
        return self.submit_limit(oid, side, new_price, new_qty, ts)

    # ---- both modes ----------------------------------------------------------------------------

    def cancel(self, oid):
        if oid == 0:
            return "InvalidId"
        if oid not in self.where:
            return "UnknownId"
        self._remove(oid)
        return "Ok"

    def reduce(self, oid, by_qty):
        if oid == 0:
            return "InvalidId"
        if by_qty <= 0:
            return "InvalidQty"
        if oid not in self.where:
            return "UnknownId"
        o = self._order(oid)
        if by_qty >= o[1]:
            self._remove(oid)
        else:
            o[1] -= by_qty
        return "Ok"

    # ---- replay mode ---------------------------------------------------------------------------

    def add_resting(self, oid, side, price, qty, ts):
        st = self._validate_new(oid, price, qty)
        if st != "Ok":
            return st
        if oid in self.where:
            return "DuplicateId"
        self._rest(oid, side, price, qty, ts)
        return "Ok"

    def execute_resting(self, oid, qty, ts):
        """Returns (status, filled, resting, fills) like the lobsim binding."""
        if oid == 0:
            return "InvalidId", 0, 0, []
        if qty <= 0:
            return "InvalidQty", 0, 0, []
        if oid not in self.where:
            return "UnknownId", 0, 0, []
        side, price = self.where[oid]
        o = self._order(oid)
        take = min(qty, o[1])
        left = o[1] - take
        if left == 0:
            self._remove(oid)
        else:
            o[1] = left
        return "Ok", take, left, [(oid, 0, side, price, take, ts, left)]

    # ---- queries -------------------------------------------------------------------------------

    def best(self, side):
        p = self._best_price(side)
        if p is None:
            return None
        q = self.levels[side][p]
        return (p, sum(o[1] for o in q), len(q))

    def depth(self, side, n):
        prices = sorted(self.levels[side], reverse=(side == BID))[:n]
        return [(p, sum(o[1] for o in self.levels[side][p]), len(self.levels[side][p]))
                for p in prices]

    def find(self, oid):
        if oid not in self.where:
            return None
        side, price = self.where[oid]
        o = self._order(oid)
        return (oid, side, price, o[1], o[2])

    def front(self, side):
        p = self._best_price(side)
        if p is None:
            return None
        o = self.levels[side][p][0]
        return (o[0], side, p, o[1], o[2])

    def queue_ahead(self, oid):
        if oid not in self.where:
            return None
        side, price = self.where[oid]
        ahead = 0
        for o in self.levels[side][price]:
            if o[0] == oid:
                return ahead
            ahead += o[1]
        raise AssertionError("index out of sync")

    def order_count(self):
        return len(self.where)

    def level_count(self, side):
        return len(self.levels[side])

    def total_qty(self, side):
        return sum(o[1] for q in self.levels[side].values() for o in q)

    def mid(self):
        b, a = self._best_price(BID), self._best_price(ASK)
        return None if b is None or a is None else 0.5 * (b + a)

    def spread(self):
        b, a = self._best_price(BID), self._best_price(ASK)
        return None if b is None or a is None else a - b
