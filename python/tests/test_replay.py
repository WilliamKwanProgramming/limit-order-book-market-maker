"""lobsim.replay_lobster: equality with the same stream applied call by call, counts, top-of-book
capture, and argument checking."""

import threading

import numpy as np
import pytest

import lobsim
from lobsim import OrderBook, Side, Status, replay_lobster
from synthetic_stream import COLUMNS, DTYPES, make_stream


def columns(s):
    return [s[c] for c in COLUMNS]


def apply_one_by_one(book, s):
    """The documented semantics, written out with the scalar API. Returns (rejected flags, top)."""
    n = len(s["type"])
    rejected = np.zeros(n, bool)
    top = np.zeros((n, 4), np.int64)
    t, ty, oid, size, price, d = (s[c].tolist() for c in COLUMNS)
    for i in range(n):
        k = ty[i]
        if k == 1:
            if d[i] in (1, -1):
                ok = book.add_resting(oid[i], Side(d[i]), price[i], size[i], t[i]) == Status.Ok
            else:
                ok = False
        elif k == 2:
            ok = book.reduce(oid[i], size[i]) == Status.Ok
        elif k == 3:
            ok = book.cancel(oid[i]) == Status.Ok
        elif k == 4:
            ok = book.execute_resting(oid[i], size[i], t[i]).status == Status.Ok
        elif k in (5, 6, 7):
            ok = True
        else:
            ok = False
        rejected[i] = not ok
        b, a = book.best_bid(), book.best_ask()
        top[i] = (b.price if b else 0, b.qty if b else 0, a.price if a else 0, a.qty if a else 0)
    return rejected, top


def full_state(book):
    orders = []
    for side in (Side.Bid, Side.Ask):
        orders.append(book.depth(side, book.level_count(side) + 1).tolist())
    return orders


@pytest.mark.parametrize("seed,noise", [(11, 0.0), (12, 0.05), (13, 0.2)])
def test_bulk_equals_call_by_call(seed, noise):
    s = make_stream(30_000, seed, noise=noise)
    n = len(s["type"])
    assert n > 60_000

    bulk = OrderBook()
    res = replay_lobster(bulk, *columns(s), capture_top=True)
    ref = OrderBook()
    rejected, top = apply_one_by_one(ref, s)

    assert res.messages == n
    assert res.rejected == int(rejected.sum())
    assert res.first_rejected == (int(np.argmax(rejected)) if rejected.any() else -1)
    np.testing.assert_array_equal(np.column_stack([res.bid_price, res.bid_qty, res.ask_price,
                                                   res.ask_qty]), top)
    assert full_state(bulk) == full_state(ref)
    assert len(bulk) == len(ref)
    for oid in np.unique(s["order_id"])[:2000].tolist():
        assert bulk.find(oid) == ref.find(oid)
        assert bulk.queue_ahead(oid) == ref.queue_ahead(oid)
    ok, why = bulk.check_invariants(require_uncrossed=False)
    assert ok, why

    # Per-type counts: index t for t = 1..7, index 0 for everything else.
    ty = s["type"].astype(np.int64)
    expected_counts = np.array([np.sum((ty < 1) | (ty > 7))] + [np.sum(ty == k)
                                                                 for k in range(1, 8)])
    np.testing.assert_array_equal(res.count_by_type, expected_counts)
    slot = np.where((ty >= 1) & (ty <= 7), ty, 0)
    np.testing.assert_array_equal(res.rejected_by_type,
                                  np.bincount(slot[rejected], minlength=8))
    assert res.rejected_by_type[5:].sum() == 0  # types 5-7 never rejected
    if noise > 0:
        assert res.rejected > 0 and res.count_by_type[0] > 0
        assert res.rejected_by_type[0] == res.count_by_type[0]
    else:
        assert res.rejected == 0
        # Sanity check of the stream itself: the book is two-sided almost all the time.
        both = (res.bid_price > 0) & (res.ask_price > 0)
        assert both.mean() > 0.9


def test_bulk_replay_in_chunks_equals_one_call():
    s = make_stream(20_000, 5, noise=0.05)
    one = OrderBook()
    r1 = replay_lobster(one, *columns(s))
    chunked = OrderBook()
    cuts = [0, 1, 777, 20_000, len(s["type"])]
    total_rejected = 0
    for a, b in zip(cuts[:-1], cuts[1:]):
        r = replay_lobster(chunked, *[c[a:b] for c in columns(s)])
        total_rejected += r.rejected
    assert total_rejected == r1.rejected
    assert full_state(one) == full_state(chunked)


def test_replay_on_existing_book_and_crossed_book():
    book = OrderBook()
    book.add_resting(7, Side.Ask, 100, 5, 0)
    s = {
        "time_ns": np.array([1, 2, 3, 4, 5, 6], np.int64),
        "type": np.array([1, 1, 4, 2, 3, 7], np.int8),
        "order_id": np.array([8, 9, 7, 8, 9, 0], np.uint64),
        "size": np.array([5, 3, 2, 1, 0, 0], np.int64),
        "price": np.array([101, 99, 0, 0, 0, -1], np.int64),
        "direction": np.array([1, 1, -1, 1, 1, 0], np.int8),
    }
    r = replay_lobster(book, *columns(s), capture_top=True)
    assert r.rejected == 0
    # After message 0 the bid 101 crosses the ask 100: replay mode allows it.
    assert (r.bid_price[0], r.ask_price[0]) == (101, 100)
    assert r.bid_price.tolist() == [101, 101, 101, 101, 101, 101]
    assert r.bid_qty.tolist() == [5, 5, 5, 4, 4, 4]
    assert r.ask_qty.tolist() == [5, 5, 3, 3, 3, 3]
    assert book.spread() == -1
    assert book.check_invariants(False)[0]
    assert not book.check_invariants(True)[0]


def test_empty_side_is_captured_as_zero():
    book = OrderBook()
    s = {c: np.zeros(2, DTYPES[c]) for c in COLUMNS}
    s["type"][:] = [1, 3]
    s["order_id"][:] = [1, 1]
    s["size"][:] = [10, 0]
    s["price"][:] = [50, 0]
    s["direction"][:] = [-1, -1]
    r = replay_lobster(book, *columns(s), capture_top=True)
    assert r.ask_price.tolist() == [50, 0] and r.ask_qty.tolist() == [10, 0]
    assert r.bid_price.tolist() == [0, 0] and r.bid_qty.tolist() == [0, 0]


def test_empty_arrays():
    book = OrderBook()
    s = {c: np.zeros(0, DTYPES[c]) for c in COLUMNS}
    r = replay_lobster(book, *columns(s), capture_top=True)
    assert (r.messages, r.rejected, r.first_rejected) == (0, 0, -1)
    assert r.count_by_type.tolist() == [0] * 8
    assert r.bid_price.shape == (0,) and r.bid_price.dtype == np.int64
    r = replay_lobster(book, *columns(s))
    assert r.bid_price is None and r.ask_qty is None
    assert len(book) == 0


def test_non_contiguous_columns_are_read_correctly():
    s = make_stream(5_000, 21, noise=0.05)
    a, b = OrderBook(), OrderBook()
    r1 = replay_lobster(a, *columns(s), capture_top=True)
    # Strided views of the same data (every column interleaved with junk).
    strided = []
    for c in columns(s):
        buf = np.zeros(2 * len(c), c.dtype)
        buf[::2] = c
        strided.append(buf[::2])
    assert not strided[0].flags["C_CONTIGUOUS"]
    r2 = replay_lobster(b, *strided, capture_top=True)
    assert r1.rejected == r2.rejected
    np.testing.assert_array_equal(r1.bid_price, r2.bid_price)
    assert full_state(a) == full_state(b)


# ---- Argument checking -------------------------------------------------------------------------

def valid_columns(n=3):
    return {c: np.ones(n, DTYPES[c]) for c in COLUMNS}


@pytest.mark.parametrize("col", COLUMNS)
def test_wrong_dtype_is_value_error(col):
    s = valid_columns()
    wrong = np.float64 if col != "price" else np.int32
    s[col] = s[col].astype(wrong)
    with pytest.raises(ValueError, match=f"'{col}' must have dtype {DTYPES[col].__name__}"):
        replay_lobster(OrderBook(), *columns(s))


def test_byte_swapped_dtype_is_value_error():
    s = valid_columns()
    s["price"] = s["price"].astype(">i8")
    with pytest.raises(ValueError, match="'price' must have dtype int64"):
        replay_lobster(OrderBook(), *columns(s))


@pytest.mark.parametrize("col", COLUMNS[1:])
def test_mismatched_lengths_is_value_error(col):
    s = valid_columns()
    s[col] = np.ones(4, DTYPES[col])
    with pytest.raises(ValueError, match=f"same length; '{col}' has length 4 but 'time_ns' has "
                                         "length 3"):
        replay_lobster(OrderBook(), *columns(s))


def test_two_dimensional_is_value_error():
    s = valid_columns()
    s["size"] = np.ones((3, 1), np.int64)
    with pytest.raises(ValueError, match="'size' must be one-dimensional"):
        replay_lobster(OrderBook(), *columns(s))


def test_non_array_is_type_error():
    s = valid_columns()
    s["order_id"] = [1, 1, 1]
    with pytest.raises(TypeError, match="'order_id' must be a numpy.ndarray, got list"):
        replay_lobster(OrderBook(), *columns(s))
    with pytest.raises(TypeError):
        replay_lobster("not a book", *columns(valid_columns()))


def test_rejected_arguments_leave_book_untouched():
    book = OrderBook()
    book.add_resting(1, Side.Bid, 10, 1)
    s = valid_columns()
    s["direction"] = s["direction"].astype(np.int64)
    with pytest.raises(ValueError):
        replay_lobster(book, *columns(s))
    assert len(book) == 1 and book.check_invariants()[0]


# ---- GIL and concurrent use ----------------------------------------------------------------

def test_replay_releases_gil_and_guards_the_book():
    s = make_stream(400_000, 3)  # roughly 0.9M messages: long enough to observe from outside
    book = OrderBook()
    result = {}

    def worker():
        result["r"] = replay_lobster(book, *columns(s))

    th = threading.Thread(target=worker)
    th.start()
    # The busy flag is set and cleared with the GIL held. If replay_lobster kept the GIL for
    # the whole loop, this thread could never run while the flag is set.
    refused = 0
    while th.is_alive():
        try:
            book.order_count()
        except RuntimeError as e:
            assert "in use by replay_lobster" in str(e)
            refused += 1
    th.join()
    assert result["r"].rejected == 0
    assert refused > 0, "the main thread never ran while the replay was in progress"
    assert book.check_invariants(False)[0]  # usable again afterwards


def test_module_exports():
    for name in ("OrderBook", "Side", "Status", "Fill", "Level", "OrderInfo", "SubmitResult",
                 "ReplayResult", "replay_lobster", "paper"):
        assert hasattr(lobsim, name)
