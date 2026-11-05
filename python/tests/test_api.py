"""Enums, value classes, argument types and the documented error behavior of the bindings."""

import enum
import pickle

import numpy as np
import pytest

from lobsim import Fill, Level, OrderBook, OrderInfo, Side, Status, SubmitResult


def test_side_enum_matches_cpp_and_lobster():
    assert issubclass(Side, enum.IntEnum)
    assert Side.Bid == 1 and Side.Ask == -1
    assert Side(1) is Side.Bid and Side(-1) is Side.Ask
    assert [m.name for m in Side] == ["Bid", "Ask"]


def test_status_enum_matches_cpp():
    assert issubclass(Status, enum.Enum) and not issubclass(Status, int)
    assert [(m.name, m.value) for m in Status] == [
        ("Ok", 0), ("InvalidId", 1), ("InvalidPrice", 2), ("InvalidQty", 3),
        ("DuplicateId", 4), ("UnknownId", 5)]


def test_each_status_is_produced_by_the_engine():
    book = OrderBook()
    seen = {
        book.add_resting(1, Side.Bid, 10, 1),
        book.add_resting(0, Side.Bid, 10, 1),
        book.add_resting(2, Side.Bid, 0, 1),
        book.add_resting(2, Side.Bid, 10, 0),
        book.add_resting(1, Side.Bid, 10, 1),
        book.cancel(99),
    }
    assert seen == set(Status)


def test_fills_report_correct_sides():
    book = OrderBook()
    book.submit_limit(1, Side.Bid, 100, 1)
    book.submit_limit(2, Side.Ask, 101, 1)
    hit_bid = book.submit_market(3, Side.Ask, 1)
    lift_ask = book.submit_market(4, Side.Bid, 1)
    assert hit_bid.fills[0].resting_side is Side.Bid and hit_bid.fills[0].price == 100
    assert lift_ask.fills[0].resting_side is Side.Ask and lift_ask.fills[0].price == 101


def test_value_classes_are_read_only_and_comparable():
    f = Fill(resting_id=1, aggressor_id=2, resting_side=Side.Ask, price=100, qty=3, ts=4,
             resting_left=5)
    assert repr(f) == ("Fill(resting_id=1, aggressor_id=2, resting_side=Side.Ask, price=100, "
                       "qty=3, ts=4, resting_left=5)")
    lv = Level(price=100, qty=3, count=2)
    assert repr(lv) == "Level(price=100, qty=3, count=2)"
    o = OrderInfo(id=7, side=Side.Bid, price=99, qty=5, ts=1)
    assert repr(o) == "OrderInfo(id=7, side=Side.Bid, price=99, qty=5, ts=1)"
    for obj, attr in [(f, "qty"), (lv, "price"), (o, "side")]:
        with pytest.raises(AttributeError):
            setattr(obj, attr, 0)
    assert lv == Level(price=100, qty=3, count=2) and lv != Level(price=100, qty=3, count=1)
    assert lv != (100, 3, 2)
    assert len({lv, Level(price=100, qty=3, count=2)}) == 1
    assert hash(o) == hash(OrderInfo(id=7, side=Side.Bid, price=99, qty=5, ts=1))


def test_submit_result_repr_and_fields():
    book = OrderBook()
    book.submit_limit(1, Side.Ask, 100, 5, 1)
    r = book.submit_limit(2, Side.Bid, 100, 7, 2)
    assert isinstance(r, SubmitResult)
    assert repr(r) == "SubmitResult(status=Status.Ok, filled=5, resting=2, fills=<1 fills>)"
    assert isinstance(r.fills, tuple)
    with pytest.raises(AttributeError):
        r.status = Status.InvalidId
    with pytest.raises(TypeError):
        SubmitResult()


def test_book_repr_and_len():
    book = OrderBook()
    assert repr(book) == "OrderBook(orders=0, best_bid=None, best_ask=None)"
    book.submit_limit(1, Side.Bid, 99, 3)
    book.submit_limit(2, Side.Ask, 101, 4)
    assert repr(book) == "OrderBook(orders=2, best_bid=3@99, best_ask=4@101)"
    assert len(book) == 2


@pytest.mark.parametrize("call", [
    lambda b: b.submit_limit(1, 1, 100, 1),            # int instead of Side
    lambda b: b.submit_limit(1, "Bid", 100, 1),
    lambda b: b.submit_limit(1, Side.Bid, 100.0, 1),   # float price
    lambda b: b.submit_limit(1, Side.Bid, 100, 1.5),
    lambda b: b.submit_limit(-1, Side.Bid, 100, 1),    # negative id does not fit uint64
    lambda b: b.submit_limit(2**64, Side.Bid, 100, 1),  # too large for uint64
    lambda b: b.submit_limit(1, Side.Bid, 2**63, 1),   # too large for int64
    lambda b: b.cancel("1"),
    lambda b: b.cancel(None),
    lambda b: b.depth(Side.Bid, 2.0),
    lambda b: b.reserve(-1, 10),
    lambda b: b.find(1.0),
])
def test_wrong_python_types_raise_type_error(call):
    book = OrderBook()
    with pytest.raises(TypeError):
        call(book)
    assert len(book) == 0


def test_numpy_integer_scalars_are_accepted():
    book = OrderBook()
    r = book.submit_limit(np.uint64(5), Side.Bid, np.int64(100), np.int32(3), np.int64(1))
    assert r.ok and book.find(np.uint64(5)).qty == 3


def test_keyword_arguments():
    book = OrderBook()
    assert book.submit_limit(id=1, side=Side.Bid, price=100, qty=2, ts=9).ok
    assert book.replace(id=1, new_price=100, new_qty=1).ok
    assert book.reduce(id=1, by_qty=1) == Status.Ok
    assert book.add_resting(id=2, side=Side.Ask, price=101, qty=1).name == "Ok"
    assert book.execute_resting(id=2, qty=1).filled == 1
    assert book.check_invariants(require_uncrossed=True) == (True, "")


def test_errors_are_statuses_not_exceptions():
    book = OrderBook()
    assert book.cancel(0) == Status.InvalidId
    assert book.reduce(5, 1) == Status.UnknownId
    assert book.replace(5, 10, 1).status == Status.UnknownId
    assert book.submit_market(1, Side.Bid, -3).status == Status.InvalidQty


def test_book_is_not_picklable():
    with pytest.raises(TypeError):
        pickle.dumps(OrderBook())


def test_docstrings_present():
    for name in ("submit_limit", "submit_market", "replace", "cancel", "reduce", "add_resting",
                 "execute_resting", "depth", "queue_ahead", "check_invariants"):
        assert getattr(OrderBook, name).__doc__
