// Unit tests for lob::OrderBook: hand-checked scenarios for every function and edge case.
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "lob/check.hpp"
#include "lob/order_book.hpp"

using lob::Fill;
using lob::kNoOrder;
using lob::Level;
using lob::OrderBook;
using lob::Side;
using lob::Status;

namespace {

constexpr Side kBid = Side::Bid;
constexpr Side kAsk = Side::Ask;

// Checked access: an empty optional is a test failure, not undefined behaviour.
template <class T>
T get(const std::optional<T>& o) {
  LOB_CHECK(o.has_value());
  return o ? *o : T{};
}

bool invariants_ok(const OrderBook& book, bool uncrossed = true) {
  std::string why;
  const bool ok = book.check_invariants(uncrossed, &why);
  if (!ok) std::fprintf(stderr, "  invariant violated: %s\n", why.c_str());
  return ok;
}

void check_fill(const Fill& f, lob::OrderId resting, lob::OrderId aggressor, Side side,
                lob::Price price, lob::Qty qty, lob::Time ts, lob::Qty left) {
  LOB_CHECK_EQ(f.resting_id, resting);
  LOB_CHECK_EQ(f.aggressor_id, aggressor);
  LOB_CHECK(f.resting_side == side);
  LOB_CHECK_EQ(f.price, price);
  LOB_CHECK_EQ(f.qty, qty);
  LOB_CHECK_EQ(f.ts, ts);
  LOB_CHECK_EQ(f.resting_left, left);
}

}  // namespace

// ---- Queries on simple books -----------------------------------------------------------------

LOB_TEST(empty_book_queries) {
  OrderBook book;
  LOB_CHECK(book.empty());
  LOB_CHECK(!book.best_bid());
  LOB_CHECK(!book.best_ask());
  LOB_CHECK(!book.mid());
  LOB_CHECK(!book.spread());
  LOB_CHECK(!book.front(kBid));
  LOB_CHECK(!book.find(1));
  LOB_CHECK(!book.find(kNoOrder));
  LOB_CHECK(!book.queue_ahead(1));
  LOB_CHECK_EQ(book.order_count(), 0u);
  LOB_CHECK_EQ(book.level_count(kBid), 0u);
  LOB_CHECK_EQ(book.total_qty(kAsk), 0);
  std::vector<Level> d{{1, 1, 1}};
  book.depth(kBid, 5, d);
  LOB_CHECK(d.empty());
  LOB_CHECK(invariants_ok(book));
}

LOB_TEST(one_sided_book_has_no_mid_or_spread) {
  OrderBook book;
  std::vector<Fill> fills;
  book.submit_limit(1, kBid, 100, 1, 0, fills);
  LOB_CHECK(book.best_bid().has_value());
  LOB_CHECK(!book.mid());
  LOB_CHECK(!book.spread());
}

LOB_TEST(resting_orders_and_top_of_book) {
  OrderBook book;
  std::vector<Fill> fills;
  LOB_CHECK(book.submit_limit(1, kBid, 100, 10, 1, fills).status == Status::Ok);
  LOB_CHECK(book.submit_limit(2, kBid, 100, 5, 2, fills).status == Status::Ok);
  LOB_CHECK(book.submit_limit(3, kBid, 99, 7, 3, fills).status == Status::Ok);
  const auto r = book.submit_limit(4, kAsk, 103, 4, 4, fills);
  LOB_CHECK(r.status == Status::Ok);
  LOB_CHECK_EQ(r.filled, 0);
  LOB_CHECK_EQ(r.resting, 4);
  LOB_CHECK(fills.empty());

  LOB_CHECK_EQ(get(book.best_bid()).price, 100);
  LOB_CHECK_EQ(get(book.best_bid()).qty, 15);
  LOB_CHECK_EQ(get(book.best_bid()).count, 2u);
  LOB_CHECK_EQ(get(book.best_ask()).price, 103);
  LOB_CHECK_EQ(get(book.best(kAsk)).qty, 4);
  LOB_CHECK_EQ(get(book.spread()), 3);
  LOB_CHECK_NEAR(get(book.mid()), 101.5, 0.0);
  LOB_CHECK_EQ(book.order_count(), 4u);
  LOB_CHECK_EQ(book.level_count(kBid), 2u);
  LOB_CHECK_EQ(book.level_count(kAsk), 1u);
  LOB_CHECK_EQ(book.total_qty(kBid), 22);
  LOB_CHECK_EQ(book.total_qty(kAsk), 4);
  LOB_CHECK_EQ(get(book.front(kBid)).id, 1u);
  LOB_CHECK_EQ(get(book.front(kAsk)).id, 4u);

  const lob::OrderInfo info = get(book.find(3));
  LOB_CHECK_EQ(info.id, 3u);
  LOB_CHECK(info.side == kBid);
  LOB_CHECK_EQ(info.price, 99);
  LOB_CHECK_EQ(info.qty, 7);
  LOB_CHECK_EQ(info.ts, 3);
  LOB_CHECK(invariants_ok(book));
}

LOB_TEST(depth_is_best_first_and_truncated) {
  OrderBook book;
  std::vector<Fill> fills;
  book.submit_limit(1, kAsk, 105, 1, 0, fills);
  book.submit_limit(2, kAsk, 102, 2, 0, fills);
  book.submit_limit(3, kAsk, 104, 3, 0, fills);
  book.submit_limit(4, kAsk, 102, 4, 0, fills);
  book.submit_limit(5, kBid, 90, 1, 0, fills);
  book.submit_limit(6, kBid, 95, 1, 0, fills);
  std::vector<Level> d;
  book.depth(kAsk, 2, d);
  LOB_CHECK_EQ(d.size(), 2u);
  LOB_CHECK_EQ(d[0].price, 102);
  LOB_CHECK_EQ(d[0].qty, 6);
  LOB_CHECK_EQ(d[0].count, 2u);
  LOB_CHECK_EQ(d[1].price, 104);
  book.depth(kAsk, 10, d);
  LOB_CHECK_EQ(d.size(), 3u);
  LOB_CHECK_EQ(d[2].price, 105);
  book.depth(kBid, 10, d);
  LOB_CHECK_EQ(d.size(), 2u);
  LOB_CHECK_EQ(d[0].price, 95);  // bids best first = descending
  LOB_CHECK_EQ(d[1].price, 90);
  book.depth(kAsk, 0, d);
  LOB_CHECK(d.empty());
}

LOB_TEST(queue_ahead_counts_shares_in_front) {
  OrderBook book;
  std::vector<Fill> fills;
  book.submit_limit(1, kBid, 100, 10, 1, fills);
  book.submit_limit(2, kBid, 100, 20, 2, fills);
  book.submit_limit(3, kBid, 100, 30, 3, fills);
  book.submit_limit(4, kBid, 99, 40, 4, fills);
  LOB_CHECK_EQ(get(book.queue_ahead(1)), 0);
  LOB_CHECK_EQ(get(book.queue_ahead(2)), 10);
  LOB_CHECK_EQ(get(book.queue_ahead(3)), 30);
  LOB_CHECK_EQ(get(book.queue_ahead(4)), 0);  // only its own level counts
  book.reduce(1, 4);
  LOB_CHECK_EQ(get(book.queue_ahead(3)), 26);
  book.cancel(2);
  LOB_CHECK_EQ(get(book.queue_ahead(3)), 6);
  LOB_CHECK(!book.queue_ahead(2));
  LOB_CHECK(!book.queue_ahead(kNoOrder));
}

// ---- Matching ----------------------------------------------------------------------------------

LOB_TEST(price_time_priority_and_partial_fill) {
  OrderBook book;
  std::vector<Fill> fills;
  book.submit_limit(1, kAsk, 101, 5, 1, fills);
  book.submit_limit(2, kAsk, 100, 5, 2, fills);
  book.submit_limit(3, kAsk, 100, 5, 3, fills);  // same price, later: behind 2
  const auto r = book.submit_limit(9, kBid, 101, 7, 10, fills);
  LOB_CHECK_EQ(r.filled, 7);
  LOB_CHECK_EQ(r.resting, 0);
  LOB_CHECK_EQ(fills.size(), 2u);
  check_fill(fills[0], 2, 9, kAsk, 100, 5, 10, 0);
  check_fill(fills[1], 3, 9, kAsk, 100, 2, 10, 3);
  LOB_CHECK(!book.find(2));
  LOB_CHECK_EQ(get(book.find(3)).qty, 3);
  LOB_CHECK_EQ(get(book.find(3)).ts, 3);  // a partial fill keeps timestamp and position
  LOB_CHECK_EQ(get(book.best_ask()).price, 100);
  LOB_CHECK_EQ(get(book.best_ask()).qty, 3);
  LOB_CHECK_EQ(get(book.best_ask()).count, 1u);
  LOB_CHECK(!book.find(9));  // fully filled aggressor never rests
  LOB_CHECK(invariants_ok(book));
}

LOB_TEST(multi_level_sweep_rests_remainder_at_limit) {
  OrderBook book;
  std::vector<Fill> fills;
  book.submit_limit(1, kBid, 100, 3, 1, fills);
  book.submit_limit(2, kBid, 99, 4, 2, fills);
  book.submit_limit(3, kBid, 98, 5, 3, fills);
  book.submit_limit(4, kBid, 97, 6, 4, fills);
  const auto r = book.submit_limit(10, kAsk, 98, 20, 5, fills);
  LOB_CHECK_EQ(r.filled, 12);
  LOB_CHECK_EQ(r.resting, 8);
  LOB_CHECK_EQ(fills.size(), 3u);
  check_fill(fills[0], 1, 10, kBid, 100, 3, 5, 0);  // resting prices, best first
  check_fill(fills[1], 2, 10, kBid, 99, 4, 5, 0);
  check_fill(fills[2], 3, 10, kBid, 98, 5, 5, 0);
  LOB_CHECK_EQ(get(book.best_ask()).price, 98);
  LOB_CHECK_EQ(get(book.best_ask()).qty, 8);
  LOB_CHECK_EQ(get(book.find(10)).ts, 5);
  LOB_CHECK_EQ(get(book.best_bid()).price, 97);
  LOB_CHECK_EQ(book.level_count(kBid), 1u);
  LOB_CHECK_EQ(book.total_qty(kBid), 6);
  LOB_CHECK(invariants_ok(book));
}

LOB_TEST(limit_at_touch_matches_and_non_crossing_rests) {
  OrderBook book;
  std::vector<Fill> fills;
  book.submit_limit(1, kAsk, 100, 5, 1, fills);
  auto r = book.submit_limit(2, kBid, 99, 5, 2, fills);
  LOB_CHECK_EQ(r.filled, 0);
  LOB_CHECK(fills.empty());
  r = book.submit_limit(3, kBid, 100, 2, 3, fills);  // equal price crosses
  LOB_CHECK_EQ(r.filled, 2);
  LOB_CHECK_EQ(fills.size(), 1u);
  check_fill(fills[0], 1, 3, kAsk, 100, 2, 3, 3);
  LOB_CHECK(invariants_ok(book));
}

LOB_TEST(fills_are_appended_not_cleared) {
  OrderBook book;
  std::vector<Fill> fills(1);  // a pre-existing element must survive
  book.submit_limit(1, kAsk, 100, 5, 1, fills);
  book.submit_market(2, kBid, 1, 2, fills);
  LOB_CHECK_EQ(fills.size(), 2u);
  LOB_CHECK_EQ(fills[0].resting_id, kNoOrder);
  LOB_CHECK_EQ(fills[1].resting_id, 1u);
}

LOB_TEST(market_order_sweeps_and_discards_rest) {
  OrderBook book;
  std::vector<Fill> fills;
  book.submit_limit(1, kAsk, 100, 3, 1, fills);
  book.submit_limit(2, kAsk, 105, 4, 2, fills);
  auto r = book.submit_market(7, kBid, 10, 3, fills);
  LOB_CHECK(r.status == Status::Ok);
  LOB_CHECK_EQ(r.filled, 7);
  LOB_CHECK_EQ(r.resting, 0);
  LOB_CHECK_EQ(fills.size(), 2u);
  check_fill(fills[0], 1, 7, kAsk, 100, 3, 3, 0);
  check_fill(fills[1], 2, 7, kAsk, 105, 4, 3, 0);
  LOB_CHECK(!book.best_ask());
  LOB_CHECK(!book.find(7));
  LOB_CHECK(book.empty());

  fills.clear();
  r = book.submit_market(8, kAsk, 5, 4, fills);
  LOB_CHECK(r.status == Status::Ok);  // empty opposite side is not an error
  LOB_CHECK_EQ(r.filled, 0);
  LOB_CHECK(fills.empty());
  LOB_CHECK(invariants_ok(book));
}

LOB_TEST(market_order_partial_level) {
  OrderBook book;
  std::vector<Fill> fills;
  book.submit_limit(1, kBid, 100, 10, 1, fills);
  const auto r = book.submit_market(2, kAsk, 4, 2, fills);
  LOB_CHECK_EQ(r.filled, 4);
  LOB_CHECK_EQ(get(book.best_bid()).qty, 6);
  check_fill(fills[0], 1, 2, kBid, 100, 4, 2, 6);
  LOB_CHECK(invariants_ok(book));
}

// ---- Cancel / reduce / replace -----------------------------------------------------------------

LOB_TEST(cancel_removes_order_and_empty_level) {
  OrderBook book;
  std::vector<Fill> fills;
  book.submit_limit(1, kBid, 100, 5, 1, fills);
  book.submit_limit(2, kBid, 100, 6, 2, fills);
  book.submit_limit(3, kBid, 99, 7, 3, fills);
  LOB_CHECK(book.cancel(1) == Status::Ok);
  LOB_CHECK_EQ(get(book.best_bid()).qty, 6);
  LOB_CHECK_EQ(get(book.front(kBid)).id, 2u);
  LOB_CHECK(book.cancel(2) == Status::Ok);
  LOB_CHECK_EQ(get(book.best_bid()).price, 99);
  LOB_CHECK_EQ(book.level_count(kBid), 1u);
  LOB_CHECK(book.cancel(2) == Status::UnknownId);  // already gone
  LOB_CHECK(book.cancel(42) == Status::UnknownId);
  LOB_CHECK(book.cancel(kNoOrder) == Status::InvalidId);
  LOB_CHECK_EQ(book.order_count(), 1u);
  LOB_CHECK(invariants_ok(book));
}

LOB_TEST(cancel_middle_of_queue) {
  OrderBook book;
  std::vector<Fill> fills;
  for (lob::OrderId id = 1; id <= 3; ++id) book.submit_limit(id, kAsk, 100, 1, 0, fills);
  book.cancel(2);
  LOB_CHECK(invariants_ok(book));
  book.submit_market(9, kBid, 2, 1, fills);
  LOB_CHECK_EQ(fills.size(), 2u);
  LOB_CHECK_EQ(fills[0].resting_id, 1u);
  LOB_CHECK_EQ(fills[1].resting_id, 3u);
}

LOB_TEST(reduce_keeps_priority_and_removes_at_zero) {
  OrderBook book;
  std::vector<Fill> fills;
  book.submit_limit(1, kAsk, 100, 10, 1, fills);
  book.submit_limit(2, kAsk, 100, 10, 2, fills);
  LOB_CHECK(book.reduce(1, 3) == Status::Ok);
  LOB_CHECK_EQ(get(book.find(1)).qty, 7);
  LOB_CHECK_EQ(get(book.best_ask()).qty, 17);
  LOB_CHECK_EQ(book.total_qty(kAsk), 17);
  LOB_CHECK_EQ(get(book.front(kAsk)).id, 1u);  // still first in the queue
  LOB_CHECK(book.reduce(1, 7) == Status::Ok);  // exactly the remainder: removed
  LOB_CHECK(!book.find(1));
  LOB_CHECK(book.reduce(2, 50) == Status::Ok);  // more than the remainder: removed
  LOB_CHECK(!book.find(2));
  LOB_CHECK(book.empty());
  LOB_CHECK(!book.best_ask());
  LOB_CHECK(invariants_ok(book));
}

LOB_TEST(reduce_rejects_bad_input) {
  OrderBook book;
  std::vector<Fill> fills;
  book.submit_limit(1, kAsk, 100, 10, 1, fills);
  LOB_CHECK(book.reduce(1, 0) == Status::InvalidQty);
  LOB_CHECK(book.reduce(1, -5) == Status::InvalidQty);
  LOB_CHECK(book.reduce(2, 1) == Status::UnknownId);
  LOB_CHECK(book.reduce(kNoOrder, 1) == Status::InvalidId);
  LOB_CHECK_EQ(get(book.find(1)).qty, 10);
}

LOB_TEST(replace_keeps_priority_when_same_price_and_not_larger) {
  OrderBook book;
  std::vector<Fill> fills;
  book.submit_limit(1, kBid, 100, 10, 1, fills);
  book.submit_limit(2, kBid, 100, 10, 2, fills);
  auto r = book.replace(1, 100, 6, 50, fills);
  LOB_CHECK(r.status == Status::Ok);
  LOB_CHECK_EQ(r.resting, 6);
  LOB_CHECK_EQ(get(book.front(kBid)).id, 1u);
  LOB_CHECK_EQ(get(book.find(1)).ts, 1);  // timestamp unchanged
  LOB_CHECK_EQ(get(book.best_bid()).qty, 16);
  r = book.replace(1, 100, 6, 51, fills);  // identical: no-op that keeps priority
  LOB_CHECK(r.status == Status::Ok);
  LOB_CHECK_EQ(get(book.front(kBid)).id, 1u);
  LOB_CHECK(fills.empty());
  LOB_CHECK(invariants_ok(book));
}

LOB_TEST(replace_size_increase_loses_priority) {
  OrderBook book;
  std::vector<Fill> fills;
  book.submit_limit(1, kBid, 100, 10, 1, fills);
  book.submit_limit(2, kBid, 100, 10, 2, fills);
  const auto r = book.replace(1, 100, 11, 60, fills);
  LOB_CHECK(r.status == Status::Ok);
  LOB_CHECK_EQ(r.resting, 11);
  LOB_CHECK_EQ(get(book.front(kBid)).id, 2u);
  LOB_CHECK_EQ(get(book.queue_ahead(1)), 10);
  LOB_CHECK_EQ(get(book.find(1)).ts, 60);
  LOB_CHECK_EQ(get(book.best_bid()).qty, 21);
  LOB_CHECK(invariants_ok(book));
}

LOB_TEST(replace_price_change_loses_priority) {
  OrderBook book;
  std::vector<Fill> fills;
  book.submit_limit(1, kAsk, 101, 10, 1, fills);
  book.submit_limit(2, kAsk, 100, 10, 2, fills);
  book.submit_limit(3, kAsk, 101, 10, 3, fills);
  // Move 2 to 101 with a smaller size: price changed, so it queues behind 1 and 3.
  const auto r = book.replace(2, 101, 5, 70, fills);
  LOB_CHECK(r.status == Status::Ok);
  LOB_CHECK_EQ(book.level_count(kAsk), 1u);
  LOB_CHECK_EQ(get(book.queue_ahead(2)), 20);
  LOB_CHECK_EQ(get(book.find(2)).ts, 70);
  LOB_CHECK(invariants_ok(book));
}

LOB_TEST(replace_that_crosses_matches) {
  OrderBook book;
  std::vector<Fill> fills;
  book.submit_limit(1, kAsk, 102, 4, 1, fills);
  book.submit_limit(2, kBid, 100, 10, 2, fills);
  const auto r = book.replace(2, 102, 10, 80, fills);
  LOB_CHECK(r.status == Status::Ok);
  LOB_CHECK_EQ(r.filled, 4);
  LOB_CHECK_EQ(r.resting, 6);
  LOB_CHECK_EQ(fills.size(), 1u);
  check_fill(fills[0], 1, 2, kAsk, 102, 4, 80, 0);
  LOB_CHECK_EQ(get(book.best_bid()).price, 102);
  LOB_CHECK(!book.best_ask());
  LOB_CHECK(invariants_ok(book));
}

LOB_TEST(replace_rejects_bad_input_without_change) {
  OrderBook book;
  std::vector<Fill> fills;
  book.submit_limit(1, kAsk, 102, 4, 1, fills);
  LOB_CHECK(book.replace(9, 102, 4, 0, fills).status == Status::UnknownId);
  LOB_CHECK(book.replace(1, 0, 4, 0, fills).status == Status::InvalidPrice);
  LOB_CHECK(book.replace(1, 102, 0, 0, fills).status == Status::InvalidQty);
  LOB_CHECK(book.replace(kNoOrder, 102, 4, 0, fills).status == Status::InvalidId);
  LOB_CHECK_EQ(get(book.find(1)).qty, 4);
  LOB_CHECK_EQ(get(book.find(1)).price, 102);
}

// ---- Validation --------------------------------------------------------------------------------

LOB_TEST(submit_rejects_bad_input_without_change) {
  OrderBook book;
  std::vector<Fill> fills;
  book.submit_limit(1, kAsk, 100, 5, 1, fills);
  LOB_CHECK(book.submit_limit(1, kBid, 90, 5, 2, fills).status == Status::DuplicateId);
  LOB_CHECK(book.submit_limit(1, kBid, 100, 5, 2, fills).status == Status::DuplicateId);
  LOB_CHECK(book.submit_limit(kNoOrder, kBid, 90, 5, 2, fills).status == Status::InvalidId);
  LOB_CHECK(book.submit_limit(2, kBid, 0, 5, 2, fills).status == Status::InvalidPrice);
  LOB_CHECK(book.submit_limit(2, kBid, -3, 5, 2, fills).status == Status::InvalidPrice);
  LOB_CHECK(book.submit_limit(2, kBid, 100, 0, 2, fills).status == Status::InvalidQty);
  LOB_CHECK(book.submit_limit(2, kBid, 100, -1, 2, fills).status == Status::InvalidQty);
  LOB_CHECK(book.submit_market(1, kBid, 5, 2, fills).status == Status::DuplicateId);
  LOB_CHECK(book.submit_market(kNoOrder, kBid, 5, 2, fills).status == Status::InvalidId);
  LOB_CHECK(book.submit_market(3, kBid, 0, 2, fills).status == Status::InvalidQty);
  LOB_CHECK(fills.empty());
  LOB_CHECK_EQ(book.order_count(), 1u);
  LOB_CHECK_EQ(get(book.find(1)).qty, 5);
  LOB_CHECK(invariants_ok(book));
}

LOB_TEST(ids_can_be_reused_after_leaving_the_book) {
  OrderBook book;
  std::vector<Fill> fills;
  book.submit_limit(1, kAsk, 100, 5, 1, fills);
  book.cancel(1);
  LOB_CHECK(book.submit_limit(1, kAsk, 101, 5, 2, fills).status == Status::Ok);
  book.submit_market(2, kBid, 5, 3, fills);
  LOB_CHECK(book.add_resting(1, kBid, 99, 5, 4) == Status::Ok);
  LOB_CHECK(invariants_ok(book));
}

LOB_TEST(status_names) {
  LOB_CHECK_EQ(std::string(lob::to_string(Status::Ok)), std::string("Ok"));
  LOB_CHECK_EQ(std::string(lob::to_string(Status::DuplicateId)), std::string("DuplicateId"));
  LOB_CHECK_EQ(std::string(lob::to_string(Status::UnknownId)), std::string("UnknownId"));
}

// ---- Replay mode -------------------------------------------------------------------------------

LOB_TEST(add_resting_never_matches_and_allows_crossed_book) {
  OrderBook book;
  LOB_CHECK(book.add_resting(1, kAsk, 100, 5, 1) == Status::Ok);
  LOB_CHECK(book.add_resting(2, kBid, 100, 5, 2) == Status::Ok);  // locked
  LOB_CHECK(book.add_resting(3, kBid, 101, 5, 3) == Status::Ok);  // crossed
  LOB_CHECK_EQ(book.order_count(), 3u);
  LOB_CHECK_EQ(get(book.spread()), -1);
  LOB_CHECK_NEAR(get(book.mid()), 100.5, 0.0);
  LOB_CHECK(invariants_ok(book, false));
  std::string why;
  LOB_CHECK(!book.check_invariants(true, &why));  // the uncrossed check must notice
  LOB_CHECK(!why.empty());
  LOB_CHECK(book.add_resting(3, kBid, 99, 5, 4) == Status::DuplicateId);
  LOB_CHECK(book.add_resting(4, kBid, 0, 5, 4) == Status::InvalidPrice);
  LOB_CHECK(book.add_resting(4, kBid, 99, 0, 4) == Status::InvalidQty);
  LOB_CHECK(book.add_resting(kNoOrder, kBid, 99, 1, 4) == Status::InvalidId);
}

LOB_TEST(execute_resting_reports_external_fill) {
  OrderBook book;
  std::vector<Fill> fills;
  book.add_resting(1, kBid, 100, 10, 1);
  book.add_resting(2, kBid, 100, 10, 2);
  LOB_CHECK(book.execute_resting(1, 4, 7, fills) == Status::Ok);
  LOB_CHECK_EQ(fills.size(), 1u);
  check_fill(fills[0], 1, kNoOrder, kBid, 100, 4, 7, 6);
  LOB_CHECK_EQ(get(book.front(kBid)).id, 1u);  // keeps priority
  LOB_CHECK_EQ(get(book.best_bid()).qty, 16);
  LOB_CHECK(book.execute_resting(1, 100, 8, fills) == Status::Ok);  // capped at remaining
  check_fill(fills[1], 1, kNoOrder, kBid, 100, 6, 8, 0);
  LOB_CHECK(!book.find(1));
  LOB_CHECK(book.execute_resting(1, 1, 9, fills) == Status::UnknownId);
  LOB_CHECK(book.execute_resting(2, 0, 9, fills) == Status::InvalidQty);
  LOB_CHECK(book.execute_resting(kNoOrder, 1, 9, fills) == Status::InvalidId);
  LOB_CHECK_EQ(fills.size(), 2u);
  LOB_CHECK(invariants_ok(book));
}

LOB_TEST(execute_resting_works_on_crossed_book_at_any_level) {
  OrderBook book;
  std::vector<Fill> fills;
  book.add_resting(1, kAsk, 100, 5, 1);
  book.add_resting(2, kBid, 102, 5, 2);
  book.add_resting(3, kBid, 98, 5, 3);
  LOB_CHECK(book.execute_resting(3, 5, 4, fills) == Status::Ok);  // not at the touch: allowed
  LOB_CHECK_EQ(book.level_count(kBid), 1u);
  LOB_CHECK(invariants_ok(book, false));
}

LOB_TEST(matching_on_crossed_book_matches_opposite_side_only) {
  OrderBook book;
  std::vector<Fill> fills;
  book.add_resting(1, kAsk, 100, 5, 1);
  book.add_resting(2, kBid, 102, 5, 2);  // crossed by replay
  const auto r = book.submit_limit(3, kBid, 100, 2, 3, fills);
  LOB_CHECK_EQ(r.filled, 2);
  LOB_CHECK_EQ(fills[0].resting_id, 1u);
  LOB_CHECK_EQ(get(book.best_bid()).price, 102);  // pre-existing cross persists
  LOB_CHECK(invariants_ok(book, false));
}

// ---- Housekeeping ------------------------------------------------------------------------------

LOB_TEST(reserve_and_clear) {
  OrderBook book;
  book.reserve(1000, 100);
  std::vector<Fill> fills;
  for (lob::OrderId id = 1; id <= 500; ++id) {
    book.submit_limit(id, id % 2 ? kBid : kAsk, id % 2 ? 1000 - static_cast<lob::Price>(id % 50)
                                                        : 1001 + static_cast<lob::Price>(id % 50),
                      1, 0, fills);
  }
  LOB_CHECK_EQ(book.order_count(), 500u);
  LOB_CHECK(fills.empty());
  LOB_CHECK(invariants_ok(book));
  book.clear();
  LOB_CHECK(book.empty());
  LOB_CHECK_EQ(book.level_count(kAsk), 0u);
  LOB_CHECK_EQ(book.total_qty(kBid), 0);
  LOB_CHECK(invariants_ok(book));
  LOB_CHECK(book.submit_limit(1, kBid, 10, 1, 0, fills).status == Status::Ok);
  LOB_CHECK(invariants_ok(book));
}

LOB_TEST(many_levels_exercise_binary_search_path) {
  // More than the linear-scan window of levels, inserted in scrambled order.
  OrderBook book;
  std::vector<Fill> fills;
  for (lob::OrderId i = 1; i <= 200; ++i) {
    const lob::Price p = 1000 + static_cast<lob::Price>((i * 37) % 200);
    book.add_resting(i, kAsk, p, 1, 0);
  }
  LOB_CHECK_EQ(book.level_count(kAsk), 200u);
  LOB_CHECK_EQ(get(book.best_ask()).price, 1000);
  for (lob::OrderId i = 1; i <= 200; i += 2) book.cancel(i);
  LOB_CHECK_EQ(book.level_count(kAsk), 100u);
  LOB_CHECK(invariants_ok(book));
  book.submit_market(999, kBid, 1000, 1, fills);
  LOB_CHECK_EQ(fills.size(), 100u);
  for (std::size_t k = 1; k < fills.size(); ++k) LOB_CHECK(fills[k].price > fills[k - 1].price);
  LOB_CHECK(book.empty());
}

LOB_TEST(id_index_survives_growth_and_many_erases) {
  OrderBook book;  // no reserve: forces several rehashes
  for (lob::OrderId i = 1; i <= 20000; ++i) book.add_resting(i * 7919, kBid, 100, 1, 0);
  for (lob::OrderId i = 1; i <= 20000; i += 3) book.cancel(i * 7919);
  LOB_CHECK(invariants_ok(book));
  for (lob::OrderId i = 1; i <= 20000; ++i) {
    const bool present = book.find(i * 7919).has_value();
    LOB_CHECK_EQ(present, (i - 1) % 3 != 0);
  }
}

LOB_TEST_MAIN()
