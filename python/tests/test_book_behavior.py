"""Hand-checked scenarios through the Python API, mirroring core/tests/test_order_book.cpp."""

import numpy as np
import pytest

from lobsim import Fill, Level, OrderBook, OrderInfo, Side, Status

BID, ASK = Side.Bid, Side.Ask


def F(resting, aggressor, side, price, qty, ts, left):
    return Fill(resting_id=resting, aggressor_id=aggressor, resting_side=side, price=price,
                qty=qty, ts=ts, resting_left=left)


def invariants_ok(book, uncrossed=True):
    ok, why = book.check_invariants(uncrossed)
    assert ok, why
    assert why == ""
    return True


# ---- Queries on simple books ---------------------------------------------------------------

def test_empty_book_queries():
    book = OrderBook()
    assert len(book) == 0
    assert book.best_bid() is None and book.best_ask() is None
    assert book.best(BID) is None
    assert book.mid() is None and book.spread() is None
    assert book.front(BID) is None
    assert book.find(1) is None and book.find(0) is None
    assert book.queue_ahead(1) is None
    assert book.order_count() == 0
    assert book.level_count(BID) == 0
    assert book.total_qty(ASK) == 0
    d = book.depth(BID, 5)
    assert d.shape == (0, 3) and d.dtype == np.int64
    assert invariants_ok(book)


def test_one_sided_book_has_no_mid_or_spread():
    book = OrderBook()
    book.submit_limit(1, BID, 100, 1, 0)
    assert book.best_bid() is not None
    assert book.mid() is None
    assert book.spread() is None


def test_resting_orders_and_top_of_book():
    book = OrderBook()
    assert book.submit_limit(1, BID, 100, 10, 1).status == Status.Ok
    assert book.submit_limit(2, BID, 100, 5, 2).status == Status.Ok
    assert book.submit_limit(3, BID, 99, 7, 3).status == Status.Ok
    r = book.submit_limit(4, ASK, 103, 4, 4)
    assert (r.status, r.filled, r.resting, r.fills) == (Status.Ok, 0, 4, ())
    assert r.ok

    assert book.best_bid() == Level(price=100, qty=15, count=2)
    assert book.best_ask() == Level(price=103, qty=4, count=1)
    assert book.best(ASK).qty == 4
    assert book.spread() == 3
    assert book.mid() == 101.5
    assert book.order_count() == 4 and len(book) == 4
    assert book.level_count(BID) == 2 and book.level_count(ASK) == 1
    assert book.total_qty(BID) == 22 and book.total_qty(ASK) == 4
    assert book.front(BID).id == 1 and book.front(ASK).id == 4
    assert book.find(3) == OrderInfo(id=3, side=BID, price=99, qty=7, ts=3)
    assert invariants_ok(book)


def test_depth_is_best_first_and_truncated():
    book = OrderBook()
    book.submit_limit(1, ASK, 105, 1, 0)
    book.submit_limit(2, ASK, 102, 2, 0)
    book.submit_limit(3, ASK, 104, 3, 0)
    book.submit_limit(4, ASK, 102, 4, 0)
    book.submit_limit(5, BID, 90, 1, 0)
    book.submit_limit(6, BID, 95, 1, 0)
    d = book.depth(ASK, 2)
    assert d.dtype == np.int64 and d.shape == (2, 3)
    assert d.tolist() == [[102, 6, 2], [104, 3, 1]]
    assert book.depth(ASK, 10).tolist() == [[102, 6, 2], [104, 3, 1], [105, 1, 1]]
    assert book.depth(BID, 10).tolist() == [[95, 1, 1], [90, 1, 1]]  # bids descending
    assert book.depth(ASK, 0).shape == (0, 3)
    with pytest.raises(ValueError, match="n must be >= 0"):
        book.depth(ASK, -1)


def test_depth_returns_an_independent_array():
    book = OrderBook()
    book.submit_limit(1, ASK, 105, 1, 0)
    d = book.depth(ASK, 1)
    d[0, 0] = 0
    assert book.depth(ASK, 1)[0, 0] == 105
    assert d.flags["C_CONTIGUOUS"]


def test_queue_ahead_counts_shares_in_front():
    book = OrderBook()
    book.submit_limit(1, BID, 100, 10, 1)
    book.submit_limit(2, BID, 100, 20, 2)
    book.submit_limit(3, BID, 100, 30, 3)
    book.submit_limit(4, BID, 99, 40, 4)
    assert [book.queue_ahead(i) for i in (1, 2, 3, 4)] == [0, 10, 30, 0]
    book.reduce(1, 4)
    assert book.queue_ahead(3) == 26
    book.cancel(2)
    assert book.queue_ahead(3) == 6
    assert book.queue_ahead(2) is None
    assert book.queue_ahead(0) is None


# ---- Matching -------------------------------------------------------------------------------

def test_price_time_priority_and_partial_fill():
    book = OrderBook()
    book.submit_limit(1, ASK, 101, 5, 1)
    book.submit_limit(2, ASK, 100, 5, 2)
    book.submit_limit(3, ASK, 100, 5, 3)  # same price, later: behind 2
    r = book.submit_limit(9, BID, 101, 7, 10)
    assert (r.filled, r.resting) == (7, 0)
    assert r.fills == (F(2, 9, ASK, 100, 5, 10, 0), F(3, 9, ASK, 100, 2, 10, 3))
    assert book.find(2) is None
    assert book.find(3).qty == 3
    assert book.find(3).ts == 3  # a partial fill keeps timestamp and position
    assert book.best_ask() == Level(price=100, qty=3, count=1)
    assert book.find(9) is None  # fully filled aggressor never rests
    assert invariants_ok(book)


def test_multi_level_sweep_rests_remainder_at_limit():
    book = OrderBook()
    book.submit_limit(1, BID, 100, 3, 1)
    book.submit_limit(2, BID, 99, 4, 2)
    book.submit_limit(3, BID, 98, 5, 3)
    book.submit_limit(4, BID, 97, 6, 4)
    r = book.submit_limit(10, ASK, 98, 20, 5)
    assert (r.filled, r.resting) == (12, 8)
    assert r.fills == (F(1, 10, BID, 100, 3, 5, 0), F(2, 10, BID, 99, 4, 5, 0),
                       F(3, 10, BID, 98, 5, 5, 0))
    assert book.best_ask() == Level(price=98, qty=8, count=1)
    assert book.find(10).ts == 5
    assert book.best_bid().price == 97
    assert book.level_count(BID) == 1
    assert book.total_qty(BID) == 6
    assert invariants_ok(book)


def test_limit_at_touch_matches_and_non_crossing_rests():
    book = OrderBook()
    book.submit_limit(1, ASK, 100, 5, 1)
    assert book.submit_limit(2, BID, 99, 5, 2).fills == ()
    r = book.submit_limit(3, BID, 100, 2, 3)  # equal price crosses
    assert r.filled == 2
    assert r.fills == (F(1, 3, ASK, 100, 2, 3, 3),)
    assert invariants_ok(book)


def test_fills_belong_to_their_call():
    # C++ appends to a caller-owned vector; in Python each result carries only its own fills.
    book = OrderBook()
    book.submit_limit(1, ASK, 100, 5, 1)
    r1 = book.submit_market(2, BID, 1, 2)
    r2 = book.submit_market(3, BID, 1, 3)
    assert r1.fills == (F(1, 2, ASK, 100, 1, 2, 4),)
    assert r2.fills == (F(1, 3, ASK, 100, 1, 3, 3),)
    assert r1.fills == r1.fills  # stable across accesses


def test_market_order_sweeps_and_discards_rest():
    book = OrderBook()
    book.submit_limit(1, ASK, 100, 3, 1)
    book.submit_limit(2, ASK, 105, 4, 2)
    r = book.submit_market(7, BID, 10, 3)
    assert (r.status, r.filled, r.resting) == (Status.Ok, 7, 0)
    assert r.fills == (F(1, 7, ASK, 100, 3, 3, 0), F(2, 7, ASK, 105, 4, 3, 0))
    assert book.best_ask() is None
    assert book.find(7) is None
    assert len(book) == 0
    r = book.submit_market(8, ASK, 5, 4)
    assert (r.status, r.filled, r.fills) == (Status.Ok, 0, ())  # empty side is not an error
    assert invariants_ok(book)


def test_market_order_partial_level():
    book = OrderBook()
    book.submit_limit(1, BID, 100, 10, 1)
    r = book.submit_market(2, ASK, 4, 2)
    assert r.filled == 4
    assert book.best_bid().qty == 6
    assert r.fills == (F(1, 2, BID, 100, 4, 2, 6),)
    assert invariants_ok(book)


# ---- Cancel / reduce / replace ----------------------------------------------------------------

def test_cancel_removes_order_and_empty_level():
    book = OrderBook()
    book.submit_limit(1, BID, 100, 5, 1)
    book.submit_limit(2, BID, 100, 6, 2)
    book.submit_limit(3, BID, 99, 7, 3)
    assert book.cancel(1) == Status.Ok
    assert book.best_bid().qty == 6
    assert book.front(BID).id == 2
    assert book.cancel(2) == Status.Ok
    assert book.best_bid().price == 99
    assert book.level_count(BID) == 1
    assert book.cancel(2) == Status.UnknownId
    assert book.cancel(42) == Status.UnknownId
    assert book.cancel(0) == Status.InvalidId
    assert book.order_count() == 1
    assert invariants_ok(book)


def test_cancel_middle_of_queue():
    book = OrderBook()
    for oid in (1, 2, 3):
        book.submit_limit(oid, ASK, 100, 1, 0)
    book.cancel(2)
    assert invariants_ok(book)
    r = book.submit_market(9, BID, 2, 1)
    assert [f.resting_id for f in r.fills] == [1, 3]


def test_reduce_keeps_priority_and_removes_at_zero():
    book = OrderBook()
    book.submit_limit(1, ASK, 100, 10, 1)
    book.submit_limit(2, ASK, 100, 10, 2)
    assert book.reduce(1, 3) == Status.Ok
    assert book.find(1).qty == 7
    assert book.best_ask().qty == 17
    assert book.total_qty(ASK) == 17
    assert book.front(ASK).id == 1
    assert book.reduce(1, 7) == Status.Ok
    assert book.find(1) is None
    assert book.reduce(2, 50) == Status.Ok
    assert book.find(2) is None
    assert len(book) == 0 and book.best_ask() is None
    assert invariants_ok(book)


def test_reduce_rejects_bad_input():
    book = OrderBook()
    book.submit_limit(1, ASK, 100, 10, 1)
    assert book.reduce(1, 0) == Status.InvalidQty
    assert book.reduce(1, -5) == Status.InvalidQty
    assert book.reduce(2, 1) == Status.UnknownId
    assert book.reduce(0, 1) == Status.InvalidId
    assert book.find(1).qty == 10


def test_replace_keeps_priority_when_same_price_and_not_larger():
    book = OrderBook()
    book.submit_limit(1, BID, 100, 10, 1)
    book.submit_limit(2, BID, 100, 10, 2)
    r = book.replace(1, 100, 6, 50)
    assert (r.status, r.filled, r.resting) == (Status.Ok, 0, 6)
    assert book.front(BID).id == 1
    assert book.find(1).ts == 1
    assert book.best_bid().qty == 16
    r = book.replace(1, 100, 6, 51)  # identical: no-op that keeps priority
    assert r.status == Status.Ok and r.fills == ()
    assert book.front(BID).id == 1
    assert invariants_ok(book)


def test_replace_size_increase_loses_priority():
    book = OrderBook()
    book.submit_limit(1, BID, 100, 10, 1)
    book.submit_limit(2, BID, 100, 10, 2)
    r = book.replace(1, 100, 11, 60)
    assert (r.status, r.resting) == (Status.Ok, 11)
    assert book.front(BID).id == 2
    assert book.queue_ahead(1) == 10
    assert book.find(1).ts == 60
    assert book.best_bid().qty == 21
    assert invariants_ok(book)


def test_replace_price_change_loses_priority():
    book = OrderBook()
    book.submit_limit(1, ASK, 101, 10, 1)
    book.submit_limit(2, ASK, 100, 10, 2)
    book.submit_limit(3, ASK, 101, 10, 3)
    r = book.replace(2, 101, 5, 70)
    assert r.status == Status.Ok
    assert book.level_count(ASK) == 1
    assert book.queue_ahead(2) == 20
    assert book.find(2).ts == 70
    assert invariants_ok(book)


def test_replace_that_crosses_matches():
    book = OrderBook()
    book.submit_limit(1, ASK, 102, 4, 1)
    book.submit_limit(2, BID, 100, 10, 2)
    r = book.replace(2, 102, 10, 80)
    assert (r.status, r.filled, r.resting) == (Status.Ok, 4, 6)
    assert r.fills == (F(1, 2, ASK, 102, 4, 80, 0),)
    assert book.best_bid().price == 102
    assert book.best_ask() is None
    assert invariants_ok(book)


def test_replace_rejects_bad_input_without_change():
    book = OrderBook()
    book.submit_limit(1, ASK, 102, 4, 1)
    assert book.replace(9, 102, 4, 0).status == Status.UnknownId
    assert book.replace(1, 0, 4, 0).status == Status.InvalidPrice
    assert book.replace(1, 102, 0, 0).status == Status.InvalidQty
    assert book.replace(0, 102, 4, 0).status == Status.InvalidId
    assert book.find(1) == OrderInfo(id=1, side=ASK, price=102, qty=4, ts=1)


# ---- Validation -------------------------------------------------------------------------------

def test_submit_rejects_bad_input_without_change():
    book = OrderBook()
    book.submit_limit(1, ASK, 100, 5, 1)
    cases = [
        (book.submit_limit(1, BID, 90, 5, 2), Status.DuplicateId),
        (book.submit_limit(1, BID, 100, 5, 2), Status.DuplicateId),
        (book.submit_limit(0, BID, 90, 5, 2), Status.InvalidId),
        (book.submit_limit(2, BID, 0, 5, 2), Status.InvalidPrice),
        (book.submit_limit(2, BID, -3, 5, 2), Status.InvalidPrice),
        (book.submit_limit(2, BID, 100, 0, 2), Status.InvalidQty),
        (book.submit_limit(2, BID, 100, -1, 2), Status.InvalidQty),
        (book.submit_market(1, BID, 5, 2), Status.DuplicateId),
        (book.submit_market(0, BID, 5, 2), Status.InvalidId),
        (book.submit_market(3, BID, 0, 2), Status.InvalidQty),
    ]
    for r, expected in cases:
        assert r.status == expected
        assert not r.ok
        assert (r.filled, r.resting, r.fills) == (0, 0, ())
    assert book.order_count() == 1
    assert book.find(1).qty == 5
    assert invariants_ok(book)


def test_ids_can_be_reused_after_leaving_the_book():
    book = OrderBook()
    book.submit_limit(1, ASK, 100, 5, 1)
    book.cancel(1)
    assert book.submit_limit(1, ASK, 101, 5, 2).status == Status.Ok
    book.submit_market(2, BID, 5, 3)
    assert book.add_resting(1, BID, 99, 5, 4) == Status.Ok
    assert invariants_ok(book)


# ---- Replay mode ------------------------------------------------------------------------------

def test_add_resting_never_matches_and_allows_crossed_book():
    book = OrderBook()
    assert book.add_resting(1, ASK, 100, 5, 1) == Status.Ok
    assert book.add_resting(2, BID, 100, 5, 2) == Status.Ok  # locked
    assert book.add_resting(3, BID, 101, 5, 3) == Status.Ok  # crossed
    assert book.order_count() == 3
    assert book.spread() == -1
    assert book.mid() == 100.5
    assert invariants_ok(book, False)
    ok, why = book.check_invariants(require_uncrossed=True)  # the uncrossed check must notice
    assert not ok and why
    assert book.add_resting(3, BID, 99, 5, 4) == Status.DuplicateId
    assert book.add_resting(4, BID, 0, 5, 4) == Status.InvalidPrice
    assert book.add_resting(4, BID, 99, 0, 4) == Status.InvalidQty
    assert book.add_resting(0, BID, 99, 1, 4) == Status.InvalidId


def test_execute_resting_reports_external_fill():
    book = OrderBook()
    book.add_resting(1, BID, 100, 10, 1)
    book.add_resting(2, BID, 100, 10, 2)
    r = book.execute_resting(1, 4, 7)
    assert (r.status, r.filled, r.resting) == (Status.Ok, 4, 6)
    assert r.fills == (F(1, 0, BID, 100, 4, 7, 6),)
    assert book.front(BID).id == 1  # keeps priority
    assert book.best_bid().qty == 16
    r = book.execute_resting(1, 100, 8)  # capped at remaining
    assert (r.filled, r.resting) == (6, 0)
    assert r.fills == (F(1, 0, BID, 100, 6, 8, 0),)
    assert book.find(1) is None
    for args, expected in [((1, 1, 9), Status.UnknownId), ((2, 0, 9), Status.InvalidQty),
                           ((0, 1, 9), Status.InvalidId)]:
        r = book.execute_resting(*args)
        assert r.status == expected and r.fills == () and (r.filled, r.resting) == (0, 0)
    assert invariants_ok(book)


def test_execute_resting_works_on_crossed_book_at_any_level():
    book = OrderBook()
    book.add_resting(1, ASK, 100, 5, 1)
    book.add_resting(2, BID, 102, 5, 2)
    book.add_resting(3, BID, 98, 5, 3)
    assert book.execute_resting(3, 5, 4).status == Status.Ok
    assert book.level_count(BID) == 1
    assert invariants_ok(book, False)


def test_matching_on_crossed_book_matches_opposite_side_only():
    book = OrderBook()
    book.add_resting(1, ASK, 100, 5, 1)
    book.add_resting(2, BID, 102, 5, 2)  # crossed by replay
    r = book.submit_limit(3, BID, 100, 2, 3)
    assert r.filled == 2 and r.fills[0].resting_id == 1
    assert book.best_bid().price == 102  # pre-existing cross persists
    assert invariants_ok(book, False)


# ---- Housekeeping -----------------------------------------------------------------------------

def test_reserve_and_clear():
    book = OrderBook()
    book.reserve(1000, 100)
    for oid in range(1, 501):
        side = BID if oid % 2 else ASK
        price = 1000 - oid % 50 if oid % 2 else 1001 + oid % 50
        assert book.submit_limit(oid, side, price, 1, 0).fills == ()
    assert book.order_count() == 500
    assert invariants_ok(book)
    book.clear()
    assert len(book) == 0
    assert book.level_count(ASK) == 0 and book.total_qty(BID) == 0
    assert invariants_ok(book)
    assert book.submit_limit(1, BID, 10, 1, 0).status == Status.Ok
    assert invariants_ok(book)


def test_many_levels_exercise_binary_search_path():
    book = OrderBook()
    for i in range(1, 201):
        book.add_resting(i, ASK, 1000 + (i * 37) % 200, 1, 0)
    assert book.level_count(ASK) == 200
    assert book.best_ask().price == 1000
    for i in range(1, 201, 2):
        book.cancel(i)
    assert book.level_count(ASK) == 100
    assert invariants_ok(book)
    r = book.submit_market(999, BID, 1000, 1)
    prices = [f.price for f in r.fills]
    assert len(prices) == 100 and prices == sorted(set(prices))
    assert len(book) == 0


def test_large_ids_and_prices_round_trip():
    book = OrderBook()
    big_id = 2**64 - 1
    big_price = 2**63 - 1
    assert book.add_resting(big_id, ASK, big_price, 2**62, -(2**63)) == Status.Ok
    assert book.find(big_id) == OrderInfo(id=big_id, side=ASK, price=big_price, qty=2**62,
                                          ts=-(2**63))
    assert book.depth(ASK, 1).tolist() == [[big_price, 2**62, 1]]
