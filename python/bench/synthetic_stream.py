"""Synthetic LOBSTER-style message streams for the lobsim tests and benchmark (no market data).

make_stream(n_orders, seed) builds a stream in which every order is added once (type 1), may get
a partial cancellation (type 2) and a partial execution (type 4), and is then removed by a deletion
(type 3, 75%) or a full execution (type 4, 25%) of its remaining size. Event times are drawn so that
each order's events come after its add, and the whole stream is sorted by time. About 300 orders
rest at once. Prices are in LOBSTER ticks (1 tick = 0.0001 USD): bids below and asks above a
slowly moving mid, in whole cents, so the book can occasionally lock or cross when the mid moves.

With noise > 0, extra messages that the replay must reject or ignore are mixed in: duplicate adds
of live ids, deletions of unknown ids, adds with direction 0 or size 0, types 5, 6 and 7 with
unusual field values, and type values outside 1..7.

Everything is drawn from numpy.random.default_rng(seed), so a stream is a pure function of its
arguments (for a given numpy version).
"""

from __future__ import annotations

import numpy as np

COLUMNS = ("time_ns", "type", "order_id", "size", "price", "direction")
DTYPES = {
    "time_ns": np.int64,
    "type": np.int8,
    "order_id": np.uint64,
    "size": np.int64,
    "price": np.int64,
    "direction": np.int8,
}


def make_stream(n_orders: int, seed: int, *, noise: float = 0.0,
                live_orders: float = 300.0) -> dict[str, np.ndarray]:
    """Return a dict of the six message columns with the dtypes replay_lobster expects."""
    rng = np.random.default_rng(seed)
    n = int(n_orders)

    # One add per time unit on average; mean lifetime `live_orders` units keeps that many alive.
    t_add = np.cumsum(rng.exponential(1.0, n))
    life = rng.exponential(live_orders, n)
    t_end = t_add + life
    ids = np.arange(1, n + 1, dtype=np.uint64)
    side = np.where(rng.random(n) < 0.5, 1, -1).astype(np.int8)
    size = (rng.integers(1, 10, n) * 100).astype(np.int64)

    # Mid: random walk in cents (a move in 2% of steps), one step per 50 time units, evaluated at each add time.
    n_buckets = int(t_add[-1] // 50) + 2 if n else 1
    walk = np.cumsum(rng.choice(np.array([-100, 0, 100]), n_buckets, p=[0.01, 0.98, 0.01]))
    mid = 5_000_000 + walk[(t_add // 50).astype(np.int64)] if n else np.zeros(0, np.int64)
    offset = 100 * rng.geometric(0.12, n)  # >= 1 cent from the mid
    price = (mid - side.astype(np.int64) * offset).astype(np.int64)

    # Optional partial events (keep at least half of the size), then the final event.
    has_pc = rng.random(n) < 0.10
    has_pe = rng.random(n) < 0.10
    pc_size = size // 4
    pe_size = size // 4
    remaining = size - np.where(has_pc, pc_size, 0) - np.where(has_pe, pe_size, 0)
    final_kind = rng.random(n)
    final_type = np.where(final_kind < 0.75, 3, 4).astype(np.int8)

    t_pc = t_add + rng.random(n) * life
    t_pe = t_add + rng.random(n) * life

    parts = [
        (t_add, np.full(n, 1, np.int8), ids, size, price, side),
        (t_pc[has_pc], np.full(has_pc.sum(), 2, np.int8), ids[has_pc], pc_size[has_pc],
         price[has_pc], side[has_pc]),
        (t_pe[has_pe], np.full(has_pe.sum(), 4, np.int8), ids[has_pe], pe_size[has_pe],
         price[has_pe], side[has_pe]),
        (t_end, final_type, ids, remaining, price, side),
    ]

    if noise > 0.0 and n:
        k = max(1, int(noise * n))
        span = float(t_add[-1])
        # Duplicate adds of a live id (between its add and its end).
        pick = rng.integers(0, n, k)
        t_dup = t_add[pick] + 0.5 * rng.random(k) * life[pick]
        parts.append((t_dup, np.full(k, 1, np.int8), ids[pick], size[pick], price[pick],
                      side[pick]))
        # Deletions, reductions and executions of ids that never existed.
        unknown = (n + 1 + rng.integers(0, 10 * n + 1, k)).astype(np.uint64)
        parts.append((rng.random(k) * span, rng.choice(np.array([2, 3, 4], np.int8), k), unknown,
                      np.full(k, 100, np.int64), np.full(k, 5_000_000, np.int64),
                      np.ones(k, np.int8)))
        # Adds with a bad direction or size (fresh ids, so only that field is wrong).
        fresh = (11 * n + 2 + np.arange(k)).astype(np.uint64)
        bad_dir = rng.random(k) < 0.5
        parts.append((rng.random(k) * span, np.full(k, 1, np.int8), fresh,
                      np.where(bad_dir, 100, 0).astype(np.int64),
                      np.full(k, 5_000_000, np.int64),
                      np.where(bad_dir, 0, 1).astype(np.int8)))
        # Types 5-7 (ignored) and out-of-range types (rejected) with odd field values.
        odd_types = rng.choice(np.array([5, 6, 7, 0, 8, -1], np.int8), k)
        parts.append((rng.random(k) * span, odd_types, rng.integers(0, n + 1, k).astype(np.uint64),
                      rng.integers(0, 500, k).astype(np.int64),
                      rng.choice(np.array([-1, 0, 1, 5_000_000]), k).astype(np.int64),
                      rng.choice(np.array([-1, 0, 1]), k).astype(np.int8)))

    t = np.concatenate([p[0] for p in parts])
    order = np.argsort(t, kind="stable")
    cols = [np.concatenate([p[j] for p in parts])[order] for j in range(6)]
    # Seconds-like float time units -> integer nanoseconds (1 unit = 1 ms here).
    cols[0] = np.round(cols[0] * 1e6).astype(np.int64)
    return {name: np.ascontiguousarray(col, dtype=DTYPES[name])
            for name, col in zip(COLUMNS, cols)}


def make_stream_with_messages(n_messages: int, seed: int, *, noise: float = 0.0) -> dict:
    """make_stream sized to give at least n_messages messages, truncated to exactly n_messages."""
    per_order = 2.2 + (4.0 * noise)
    s = make_stream(int(n_messages / per_order) + 1000, seed, noise=noise)
    while len(s["type"]) < n_messages:  # pragma: no cover - the estimate above is generous
        per_order *= 0.9
        s = make_stream(int(n_messages / per_order) + 1000, seed, noise=noise)
    return {k: v[:n_messages].copy() for k, v in s.items()}
