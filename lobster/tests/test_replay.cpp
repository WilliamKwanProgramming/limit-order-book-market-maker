// Replayer, validator, stats and session tests on tiny hand-built fixtures. Every expected book
// state and statistic below was worked out by hand from the messages next to it.
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "lob/check.hpp"
#include "lob/lobster_session.hpp"

using namespace lob;
using namespace lob::lobster;

namespace {

constexpr Time kS = kNanosPerSecond;

Message M(Time t, std::int32_t type, std::int64_t id, Qty size, Price price, std::int64_t dir) {
  Message m;
  m.time = t;
  m.type = type;
  m.id = id;
  m.size = size;
  m.price = price;
  m.direction = dir;
  return m;
}

using Levels = std::vector<std::pair<Price, Qty>>;

Levels levels_of(const OrderBook& b, Side s) {
  std::vector<Level> d;
  b.depth(s, 100, d);
  Levels out;
  for (const Level& l : d) out.emplace_back(l.price, l.qty);
  return out;
}

OrderInfo front_of(const Replayer& r, Side s) {
  const auto f = r.book().front(s);
  return f ? *f : OrderInfo{};
}

std::size_t idx(Anomaly a) { return static_cast<std::size_t>(a); }

Replayer lenient() { return Replayer(Replayer::Config{Policy::Lenient, false}); }

SessionResult session(const std::string& msgs, const std::string* book, SessionOptions opt = {}) {
  StringSource ms(msgs);
  if (book == nullptr) return run_session(ms, nullptr, opt);
  StringSource bs(*book);
  return run_session(ms, &bs, opt);
}

}  // namespace

// ---- Replay semantics ---------------------------------------------------------------------------

LOB_TEST(type1_adds_on_the_direction_side) {
  Replayer r;
  r.apply(M(1, 1, 10, 100, 1000000, 1), 1, 1);
  r.apply(M(2, 1, 11, 50, 1000100, -1), 2, 2);
  r.apply(M(3, 1, 12, 30, 1000000, 1), 3, 3);
  LOB_CHECK(levels_of(r.book(), Side::Bid) == (Levels{{1000000, 130}}));
  LOB_CHECK(levels_of(r.book(), Side::Ask) == (Levels{{1000100, 50}}));
  const Time ts10 = r.book().find(10) ? r.book().find(10)->ts : -1;
  LOB_CHECK_EQ(ts10, Time{1});
  LOB_CHECK_EQ(r.counters().by_type[1], std::uint64_t{3});
  LOB_CHECK_EQ(r.counters().total_anomalies(), std::uint64_t{0});
}

LOB_TEST(type2_reduces_and_keeps_priority) {
  Replayer r;
  r.apply(M(1, 1, 10, 100, 1000000, 1), 1, 1);
  r.apply(M(2, 1, 11, 40, 1000000, 1), 2, 2);
  r.apply(M(3, 2, 10, 30, 1000000, 1), 3, 3);
  LOB_CHECK(levels_of(r.book(), Side::Bid) == (Levels{{1000000, 110}}));
  LOB_CHECK_EQ(front_of(r, Side::Bid).id, OrderId{10});
  LOB_CHECK_EQ(front_of(r, Side::Bid).qty, Qty{70});
  LOB_CHECK_EQ(r.counters().total_anomalies(), std::uint64_t{0});
}

LOB_TEST(type3_deletes) {
  Replayer r;
  r.apply(M(1, 1, 10, 100, 1000000, 1), 1, 1);
  r.apply(M(2, 1, 11, 40, 999900, 1), 2, 2);
  r.apply(M(3, 3, 10, 100, 1000000, 1), 3, 3);
  LOB_CHECK(levels_of(r.book(), Side::Bid) == (Levels{{999900, 40}}));
  LOB_CHECK(!r.book().find(10));
  LOB_CHECK_EQ(r.counters().total_anomalies(), std::uint64_t{0});
}

LOB_TEST(type4_executes_partially_then_fully) {
  Replayer r;
  r.apply(M(1, 1, 10, 100, 1000100, -1), 1, 1);
  r.apply(M(2, 1, 11, 40, 1000100, -1), 2, 2);
  r.apply(M(3, 4, 10, 60, 1000100, -1), 3, 3);
  LOB_CHECK(levels_of(r.book(), Side::Ask) == (Levels{{1000100, 80}}));
  r.apply(M(3, 4, 10, 40, 1000100, -1), 4, 4);  // same timestamp: one sweep, several rows
  r.apply(M(3, 4, 11, 15, 1000100, -1), 5, 5);
  LOB_CHECK(levels_of(r.book(), Side::Ask) == (Levels{{1000100, 25}}));
  LOB_CHECK_EQ(front_of(r, Side::Ask).id, OrderId{11});
  LOB_CHECK_EQ(r.counters().total_anomalies(), std::uint64_t{0});
}

LOB_TEST(types_5_6_7_leave_the_book_unchanged) {
  Replayer r;
  r.apply(M(1, 1, 10, 100, 1000000, 1), 1, 1);
  r.apply(M(1, 1, 11, 50, 1000100, -1), 2, 2);
  const Levels bids = levels_of(r.book(), Side::Bid);
  const Levels asks = levels_of(r.book(), Side::Ask);
  // Hidden execution reusing a resting id, cross trade, halt with odd fields.
  r.apply(M(2, 5, 10, 100, 1000000, 1), 3, 3);
  r.apply(M(3, 6, 0, 5000, 1000050, -1), 4, 4);
  r.apply(M(4, 7, 0, 0, -1, 0), 5, 5);
  r.apply(M(5, 7, 0, 0, 1, 2), 6, 6);
  LOB_CHECK(levels_of(r.book(), Side::Bid) == bids);
  LOB_CHECK(levels_of(r.book(), Side::Ask) == asks);
  LOB_CHECK_EQ(r.counters().by_type[5], std::uint64_t{1});
  LOB_CHECK_EQ(r.counters().by_type[6], std::uint64_t{1});
  LOB_CHECK_EQ(r.counters().by_type[7], std::uint64_t{2});
  LOB_CHECK_EQ(r.counters().total_anomalies(), std::uint64_t{0});
  LOB_CHECK(!r.stopped());
}

// ---- Anomalies ----------------------------------------------------------------------------------

LOB_TEST(anomaly_unknown_id) {
  for (int type = 2; type <= 4; ++type) {
    Replayer r = lenient();
    r.apply(M(1, 1, 10, 100, 1000000, 1), 1, 1);
    const AnomalyMask mask = r.apply(M(2, type, 99, 10, 1000000, 1), 2, 2);
    LOB_CHECK_EQ(mask, bit(Anomaly::UnknownId));
    LOB_CHECK_EQ(r.counters().anomalies[idx(Anomaly::UnknownId)], std::uint64_t{1});
    LOB_CHECK_EQ(r.counters().skipped_engine_calls, std::uint64_t{1});
    LOB_CHECK_EQ(r.counters().engine_rejections, std::uint64_t{0});
    LOB_CHECK(levels_of(r.book(), Side::Bid) == (Levels{{1000000, 100}}));
  }
}

LOB_TEST(anomaly_duplicate_id) {
  Replayer r = lenient();
  r.apply(M(1, 1, 10, 100, 1000000, 1), 1, 1);
  const AnomalyMask mask = r.apply(M(2, 1, 10, 7, 1000100, -1), 2, 2);
  LOB_CHECK_EQ(mask, bit(Anomaly::DuplicateId));
  LOB_CHECK_EQ(r.counters().anomalies[idx(Anomaly::DuplicateId)], std::uint64_t{1});
  LOB_CHECK(levels_of(r.book(), Side::Ask).empty());  // skipped
  LOB_CHECK_EQ(r.counters().skipped_engine_calls, std::uint64_t{1});
}

LOB_TEST(anomaly_size_exceeds_remaining) {
  Replayer r = lenient();
  r.apply(M(1, 1, 10, 100, 1000000, 1), 1, 1);
  r.apply(M(1, 1, 11, 100, 1000000, 1), 2, 2);
  LOB_CHECK_EQ(r.apply(M(2, 2, 10, 150, 1000000, 1), 3, 3), bit(Anomaly::SizeExceedsRemaining));
  LOB_CHECK_EQ(r.apply(M(3, 4, 11, 101, 1000000, 1), 4, 4), bit(Anomaly::SizeExceedsRemaining));
  LOB_CHECK_EQ(r.counters().anomalies[idx(Anomaly::SizeExceedsRemaining)], std::uint64_t{2});
  // Lenient: the engine caps the size, so both orders are gone.
  LOB_CHECK(r.book().empty());
  LOB_CHECK_EQ(r.counters().skipped_engine_calls, std::uint64_t{0});
}

LOB_TEST(anomaly_partial_cancel_removes_order) {
  Replayer r = lenient();
  r.apply(M(1, 1, 10, 100, 1000000, 1), 1, 1);
  LOB_CHECK_EQ(r.apply(M(2, 2, 10, 100, 1000000, 1), 2, 2),
               bit(Anomaly::PartialCancelRemovesOrder));
  LOB_CHECK_EQ(r.counters().anomalies[idx(Anomaly::PartialCancelRemovesOrder)], std::uint64_t{1});
  LOB_CHECK(r.book().empty());
}

LOB_TEST(anomaly_field_mismatches) {
  Replayer r = lenient();
  r.apply(M(1, 1, 10, 100, 1000000, 1), 1, 1);
  r.apply(M(1, 1, 11, 100, 1000100, -1), 2, 2);
  r.apply(M(1, 1, 12, 100, 999900, 1), 3, 3);
  LOB_CHECK_EQ(r.apply(M(2, 4, 10, 10, 1000000, -1), 4, 4), bit(Anomaly::SideMismatch));
  LOB_CHECK_EQ(r.apply(M(2, 2, 11, 10, 1000200, -1), 5, 5), bit(Anomaly::PriceMismatch));
  LOB_CHECK_EQ(r.apply(M(2, 3, 12, 60, 999900, 1), 6, 6), bit(Anomaly::DeleteSizeMismatch));
  LOB_CHECK_EQ(r.apply(M(2, 3, 10, 5, 1000300, -1), 7, 7),
               bit(Anomaly::SideMismatch) | bit(Anomaly::PriceMismatch) |
                   bit(Anomaly::DeleteSizeMismatch));
  const ReplayCounters& c = r.counters();
  LOB_CHECK_EQ(c.anomalies[idx(Anomaly::SideMismatch)], std::uint64_t{2});
  LOB_CHECK_EQ(c.anomalies[idx(Anomaly::PriceMismatch)], std::uint64_t{2});
  LOB_CHECK_EQ(c.anomalies[idx(Anomaly::DeleteSizeMismatch)], std::uint64_t{2});
  LOB_CHECK_EQ(c.field_mismatches(), std::uint64_t{6});
  LOB_CHECK_EQ(c.messages_with_anomaly, std::uint64_t{4});
  // Lenient applies by id: 10 executed 10 then deleted, 11 reduced by 10, 12 deleted.
  LOB_CHECK(levels_of(r.book(), Side::Ask) == (Levels{{1000100, 90}}));
  LOB_CHECK(levels_of(r.book(), Side::Bid).empty());
}

LOB_TEST(anomaly_time_backwards) {
  Replayer r = lenient();
  r.apply(M(5 * kS, 1, 10, 100, 1000000, 1), 1, 1);
  LOB_CHECK_EQ(r.apply(M(5 * kS, 7, 0, 0, -1, 0), 2, 2), AnomalyMask{0});  // equal is fine
  LOB_CHECK_EQ(r.apply(M(5 * kS - 1, 1, 11, 1, 1000100, -1), 3, 3), bit(Anomaly::TimeBackwards));
  LOB_CHECK_EQ(r.counters().anomalies[idx(Anomaly::TimeBackwards)], std::uint64_t{1});
  LOB_CHECK(r.book().find(11).has_value());  // still applied under lenient
}

LOB_TEST(strict_stops_at_first_anomaly_without_applying) {
  Replayer r;  // strict by default
  LOB_CHECK(r.config().policy == Policy::Strict);
  r.apply(M(1, 1, 10, 100, 1000000, 1), 1, 11);
  r.apply(M(2, 2, 10, 100, 1000000, 1), 2, 12);  // removes the whole order: anomaly
  LOB_CHECK(r.stopped());
  LOB_CHECK(levels_of(r.book(), Side::Bid) == (Levels{{1000000, 100}}));  // not applied
  LOB_CHECK(r.first_anomaly().set);
  LOB_CHECK_EQ(r.first_anomaly().index, std::uint64_t{2});
  LOB_CHECK_EQ(r.first_anomaly().line, std::uint64_t{12});
  LOB_CHECK_EQ(r.first_anomaly().message.type, 2);
  LOB_CHECK_EQ(r.first_anomaly().mask, bit(Anomaly::PartialCancelRemovesOrder));
  LOB_CHECK_EQ(r.apply(M(3, 1, 20, 5, 1000100, -1), 3, 13), AnomalyMask{0});
  LOB_CHECK(!r.book().find(20));
  LOB_CHECK_EQ(r.counters().messages, std::uint64_t{2});
}

LOB_TEST(lenient_continues_after_anomalies) {
  Replayer r = lenient();
  r.apply(M(1, 3, 99, 5, 1000000, 1), 1, 1);
  r.apply(M(2, 1, 10, 100, 1000000, 1), 2, 2);
  LOB_CHECK(!r.stopped());
  LOB_CHECK_EQ(r.first_anomaly().index, std::uint64_t{1});
  LOB_CHECK(levels_of(r.book(), Side::Bid) == (Levels{{1000000, 100}}));
}

// ---- Bootstrap (ghost orders) -------------------------------------------------------------------

// A window that starts mid-day. Before it, the book held bids 1000000 x 100 (orders 5: 60 and
// 6: 40) and 999900 x 50 (order 7), and asks 1000100 x 30 (order 8). None of these ids appear as
// type 1 in the window.
LOB_TEST(bootstrap_resolves_references_to_earlier_orders) {
  const std::string msgs =
      "36000.1,2,5,10,1000000,1\n"   // partial cancel of pre-existing 5: bid 100 -> 90
      "36000.2,1,20,5,1000000,1\n"   // new bid behind the ghost: 95
      "36000.3,4,6,40,1000000,1\n"   // execution of pre-existing 6: 55
      "36000.4,3,5,50,1000000,1\n"   // deletion of the rest of 5: 5 (order 20 only)
      "36000.5,3,99,10,1000200,-1\n" // unknown order deeper than the visible book: unexplained
      "36000.6,4,7,50,999900,1\n";   // execution of 7 empties the second bid level
  const std::string book =
      "1000100,30,1000000,90,9999999999,0,999900,50\n"
      "1000100,30,1000000,95,9999999999,0,999900,50\n"
      "1000100,30,1000000,55,9999999999,0,999900,50\n"
      "1000100,30,1000000,5,9999999999,0,999900,50\n"
      "1000100,30,1000000,5,9999999999,0,999900,50\n"
      "1000100,30,1000000,5,9999999999,0,-9999999999,0\n";
  SessionOptions opt;
  opt.policy = Policy::Lenient;
  opt.bootstrap = true;
  const SessionResult r = session(msgs, &book, opt);
  const GhostCounters& g = r.ghosts;
  LOB_CHECK(g.seeded);
  LOB_CHECK(g.first_message_undone);
  LOB_CHECK_EQ(g.created, std::uint64_t{3});
  LOB_CHECK_EQ(g.created_qty, Qty{180});
  LOB_CHECK_EQ(g.references_resolved, std::uint64_t{4});
  LOB_CHECK_EQ(g.consumed_qty, Qty{150});
  LOB_CHECK_EQ(g.emptied, std::uint64_t{2});
  LOB_CHECK_EQ(g.unexplained, std::uint64_t{1});
  LOB_CHECK_EQ(g.shortfall, std::uint64_t{0});
  LOB_CHECK_EQ(r.replay.anomalies[idx(Anomaly::UnknownId)], std::uint64_t{1});
  LOB_CHECK_EQ(r.validation.checked, std::uint64_t{6});
  LOB_CHECK_EQ(r.validation.mismatched, std::uint64_t{0});
  LOB_CHECK_EQ(r.exit_code(), 0);
  LOB_CHECK_EQ(r.final_orders, std::size_t{2});  // order 20 and the ask ghost

  // Without the bootstrap the first message is already an unknown id; strict stops there.
  const SessionResult s = session(msgs, &book);
  LOB_CHECK(s.stopped_early);
  LOB_CHECK_EQ(s.first_anomaly.index, std::uint64_t{1});
  LOB_CHECK_EQ(s.exit_code(), 1);
  // Lenient without bootstrap: every row mismatches.
  SessionOptions len;
  len.policy = Policy::Lenient;
  const SessionResult l = session(msgs, &book, len);
  LOB_CHECK_EQ(l.validation.mismatched, std::uint64_t{6});
  LOB_CHECK_EQ(l.replay.anomalies[idx(Anomaly::UnknownId)], std::uint64_t{5});
}

LOB_TEST(bootstrap_undoes_first_message_and_reports_shortfall) {
  // First message adds 20 at the best ask; the level held 30 before it.
  const std::string msgs =
      "36000.1,1,50,20,1000100,-1\n"
      "36000.2,4,8,40,1000100,-1\n";  // pre-existing ask(s) at 1000100: 30, but 40 executed
  const std::string book =
      "1000100,50,1000000,10\n"
      "1000100,20,1000000,10\n";
  SessionOptions opt;
  opt.policy = Policy::Lenient;
  opt.bootstrap = true;
  const SessionResult r = session(msgs, &book, opt);
  LOB_CHECK_EQ(r.ghosts.created, std::uint64_t{2});
  LOB_CHECK_EQ(r.ghosts.created_qty, Qty{40});  // ask 30 + bid 10
  LOB_CHECK_EQ(r.ghosts.shortfall, std::uint64_t{1});
  LOB_CHECK_EQ(r.replay.anomalies[idx(Anomaly::SizeExceedsRemaining)], std::uint64_t{1});
  // The ghost gives up 30; order 50 (20) remains, which is what the file shows.
  LOB_CHECK_EQ(r.validation.mismatched, std::uint64_t{0});
  // Strict with bootstrap stops at the shortfall.
  opt.policy = Policy::Strict;
  const SessionResult s = session(msgs, &book, opt);
  LOB_CHECK(s.stopped_early);
  LOB_CHECK_EQ(s.first_anomaly.index, std::uint64_t{2});
}

LOB_TEST(bootstrap_first_message_inconsistent_with_row) {
  // A type 1 of size 20 at a price whose level shows only 10: the row cannot follow the message.
  Replayer r(Replayer::Config{Policy::Lenient, true});
  BookRow row;
  row.resize(1);
  row.asks[0] = {1000100, 10};
  row.bids[0] = {1000000, 10};
  LOB_CHECK(r.seed_ghosts(M(1, 1, 50, 20, 1000100, -1), row));
  LOB_CHECK(!r.ghosts().first_message_undone);
  LOB_CHECK(!r.seed_ghosts(M(1, 1, 50, 20, 1000100, -1), row));  // only once
  Replayer off;
  LOB_CHECK(!off.seed_ghosts(M(1, 1, 50, 20, 1000100, -1), row));  // bootstrap off
  LOB_CHECK(off.book().empty());
}

// ---- Validator ----------------------------------------------------------------------------------

namespace {

const char* kMsgs =
    "34200.1,1,1,100,1000000,1\n"
    "34200.2,1,2,50,1000100,-1\n"
    "34200.3,1,3,70,999900,1\n"
    "34200.4,5,0,10,1000000,1\n"
    "34200.5,4,1,30,1000000,1\n"
    "34200.6,7,0,0,-1,0\n"
    "34200.7,3,2,50,1000100,-1\n";

std::vector<std::string> book_rows() {
  return {
      "9999999999,0,1000000,100,9999999999,0,-9999999999,0",
      "1000100,50,1000000,100,9999999999,0,-9999999999,0",
      "1000100,50,1000000,100,9999999999,0,999900,70",
      "1000100,50,1000000,100,9999999999,0,999900,70",
      "1000100,50,1000000,70,9999999999,0,999900,70",
      "1000100,50,1000000,70,9999999999,0,999900,70",
      "9999999999,0,1000000,70,9999999999,0,999900,70",
  };
}

std::string join(const std::vector<std::string>& rows) {
  std::string s;
  for (const auto& r : rows) s += r + "\n";
  return s;
}

}  // namespace

LOB_TEST(validator_clean_fixture) {
  const std::string book = join(book_rows());
  SessionOptions opt;
  opt.stats = true;
  const SessionResult r = session(kMsgs, &book, opt);
  LOB_CHECK(r.fatal.empty());
  LOB_CHECK_EQ(r.levels, std::size_t{2});
  LOB_CHECK_EQ(r.validation.checked, std::uint64_t{7});
  LOB_CHECK_EQ(r.validation.mismatched, std::uint64_t{0});
  LOB_CHECK_EQ(r.validation.unchanged_checked, std::uint64_t{2});
  LOB_CHECK_EQ(r.validation.unchanged_violations, std::uint64_t{0});
  LOB_CHECK_EQ(r.replay.total_anomalies(), std::uint64_t{0});
  LOB_CHECK_EQ(r.exit_code(), 0);
}

LOB_TEST(validator_flags_corrupted_size_at_that_message) {
  auto rows = book_rows();
  rows[1] = "1000100,51,1000000,100,9999999999,0,-9999999999,0";  // ask size 50 -> 51
  const std::string book = join(rows);
  const SessionResult r = session(kMsgs, &book);
  const ValidationReport& v = r.validation;
  LOB_CHECK_EQ(v.mismatched, std::uint64_t{1});
  LOB_CHECK_EQ(v.size_only, std::uint64_t{1});
  LOB_CHECK_EQ(v.price_structure, std::uint64_t{0});
  LOB_CHECK_EQ(v.ask_level_mismatches[0], std::uint64_t{1});
  LOB_CHECK_EQ(v.ask_level_mismatches[1] + v.bid_level_mismatches[0] + v.bid_level_mismatches[1],
               std::uint64_t{0});
  LOB_CHECK(v.first.set);
  LOB_CHECK_EQ(v.first.index, std::uint64_t{2});
  LOB_CHECK_EQ(v.first.line, std::uint64_t{2});
  LOB_CHECK_EQ(v.last_mismatch_index, std::uint64_t{2});
  LOB_CHECK_EQ(v.first.expected.asks[0].qty, Qty{51});
  LOB_CHECK_EQ(v.first.actual.asks[0].qty, Qty{50});
  LOB_CHECK_EQ(v.unchanged_violations, std::uint64_t{0});
  LOB_CHECK_EQ(r.exit_code(), 1);
}

LOB_TEST(validator_price_structure_and_unchanged_rows) {
  auto rows = book_rows();
  rows[2] = "1000100,50,1000000,100,9999999999,0,999800,70";  // level-2 bid price wrong
  const std::string book = join(rows);
  const SessionResult r = session(kMsgs, &book);
  const ValidationReport& v = r.validation;
  // Row 3 differs from the engine; row 4 (type 5) differs from row 3 and from the engine.
  LOB_CHECK_EQ(v.mismatched, std::uint64_t{1});
  LOB_CHECK_EQ(v.first.index, std::uint64_t{3});
  LOB_CHECK_EQ(v.price_structure, std::uint64_t{1});
  LOB_CHECK_EQ(v.bid_level_mismatches[1], std::uint64_t{1});
  LOB_CHECK_EQ(v.unchanged_violations, std::uint64_t{1});
  LOB_CHECK_EQ(v.first_unchanged_violation_index, std::uint64_t{4});

  // A type 7 row that changes the book: both checks fire at message 6.
  rows = book_rows();
  rows[5] = "1000100,50,1000000,70,9999999999,0,-9999999999,0";
  const std::string book2 = join(rows);
  const SessionResult r2 = session(kMsgs, &book2);
  LOB_CHECK_EQ(r2.validation.mismatched, std::uint64_t{1});
  LOB_CHECK_EQ(r2.validation.first.index, std::uint64_t{6});
  LOB_CHECK_EQ(r2.validation.unchanged_violations, std::uint64_t{1});
  LOB_CHECK_EQ(r2.validation.first_unchanged_violation_index, std::uint64_t{6});
}

LOB_TEST(validator_empty_side_versus_sentinel) {
  auto rows = book_rows();
  rows[6] = "1000100,50,1000000,70,9999999999,0,999900,70";  // ask should be empty
  const std::string book = join(rows);
  const SessionResult r = session(kMsgs, &book);
  LOB_CHECK_EQ(r.validation.mismatched, std::uint64_t{1});
  LOB_CHECK_EQ(r.validation.price_structure, std::uint64_t{1});
  LOB_CHECK_EQ(r.validation.first.index, std::uint64_t{7});
  LOB_CHECK(r.validation.first.actual.asks[0].empty());
}

// ---- Rows in the real-file form (sources are cited in test_format.cpp) --------------------------

LOB_TEST(session_real_format_rows_leave_the_book_unchanged) {
  // Two invented type-1 rows build a one-level book; the seven rows after them take the form that
  // public sources show for real files: an opening cross (type 6, id -1), hidden executions
  // (type 5, id 0) with direction -1 and +1, halt / quoting / resume rows (type 7, price -1, 0, 1,
  // direction -1) and a closing cross (id -2). Some message and order-book rows end in a comma.
  const std::string msgs =
      "34200.1,1,101,100,1002400,1\n"
      "34200.2,1,102,200,1002500,-1,\n"
      "34200.25,6,-1,1500,1002450,-1,\n"
      "34200.3,5,0,100,1002500,-1\n"
      "34200.4,5,0,23,1002400,1,\n"
      "36023,7,0,0,-1,-1,\n"
      "36323,7,0,0,0,-1\n"
      "36723,7,0,0,1,-1,\n"
      "57600,6,-2,2400,1002600,-1\n";
  std::vector<std::string> rows(9, "1002500,200,1002400,100");
  rows[0] = "9999999999,0,1002400,100";
  rows[3] += ",";
  rows[6] += ",";
  SessionOptions opt;  // strict policy
  opt.stats = true;
  const std::string book = join(rows);
  const SessionResult r = session(msgs, &book, opt);
  LOB_CHECK(r.fatal.empty());
  LOB_CHECK_EQ(r.message_parse_errors + r.book_parse_errors, std::uint64_t{0});
  LOB_CHECK_EQ(r.replay.messages, std::uint64_t{9});
  LOB_CHECK_EQ(r.replay.total_anomalies(), std::uint64_t{0});
  LOB_CHECK_EQ(r.final_orders, std::size_t{2});
  LOB_CHECK_EQ(r.validation.checked, std::uint64_t{9});
  LOB_CHECK_EQ(r.validation.mismatched, std::uint64_t{0});
  LOB_CHECK_EQ(r.validation.unchanged_checked, std::uint64_t{7});
  LOB_CHECK_EQ(r.validation.unchanged_violations, std::uint64_t{0});
  LOB_CHECK_EQ(r.exit_code(), 0);
  // Counted by type; hidden volume split by the direction written in the file.
  LOB_CHECK_EQ(r.stats.by_type[5], std::uint64_t{2});
  LOB_CHECK_EQ(r.stats.by_type[6], std::uint64_t{2});
  LOB_CHECK_EQ(r.stats.by_type[7], std::uint64_t{3});
  LOB_CHECK_EQ(r.stats.hidden_buy, Qty{23});
  LOB_CHECK_EQ(r.stats.hidden_sell, Qty{100});
  LOB_CHECK_EQ(r.stats.hidden_other, Qty{0});
  LOB_CHECK_EQ(r.stats.executed_bid + r.stats.executed_ask, Qty{0});
  LOB_CHECK_EQ(r.stats.best_quote_changes, std::uint64_t{2});

  // The order-book row of the resume message changed: it differs from the row before it and from
  // the engine (message 8), and the closing-cross row after it differs from it (message 9).
  rows[7] = "1002500,150,1002400,100";
  const std::string changed = join(rows);
  const SessionResult c = session(msgs, &changed, opt);
  LOB_CHECK_EQ(c.validation.unchanged_violations, std::uint64_t{2});
  LOB_CHECK_EQ(c.validation.first_unchanged_violation_index, std::uint64_t{8});
  LOB_CHECK_EQ(c.validation.mismatched, std::uint64_t{1});
  LOB_CHECK_EQ(c.exit_code(), 1);
}

// ---- Session: alignment, malformed rows, exit codes ---------------------------------------------

LOB_TEST(session_alignment_errors_are_fatal) {
  auto rows = book_rows();
  rows.pop_back();
  const std::string shorter = join(rows);
  LOB_CHECK_EQ(session(kMsgs, &shorter).exit_code(), 2);
  rows = book_rows();
  rows.push_back(rows.back());
  const std::string longer = join(rows);
  const SessionResult r = session(kMsgs, &longer);
  LOB_CHECK(!r.fatal.empty());
  LOB_CHECK_EQ(r.exit_code(), 2);
  LOB_CHECK_EQ(session("", nullptr).exit_code(), 2);
  LOB_CHECK_EQ(session("\n  \n", nullptr).exit_code(), 2);
}

LOB_TEST(session_malformed_rows_strict_and_lenient) {
  std::string msgs = kMsgs;
  msgs.replace(msgs.find("34200.6,7,0,0,-1,0"), 18, "34200.6,7,0,0,-1");  // 5 fields
  const std::string book = join(book_rows());
  const SessionResult s = session(msgs, &book);
  LOB_CHECK(s.stopped_early);
  LOB_CHECK_EQ(s.message_parse_errors, std::uint64_t{1});
  LOB_CHECK_EQ(s.first_message_error.line, std::uint64_t{6});
  LOB_CHECK(s.first_message_error.error.code == ParseCode::WrongFieldCount);
  LOB_CHECK_EQ(s.exit_code(), 1);

  SessionOptions opt;
  opt.policy = Policy::Lenient;
  const SessionResult l = session(msgs, &book, opt);
  LOB_CHECK(!l.stopped_early);
  LOB_CHECK_EQ(l.message_parse_errors, std::uint64_t{1});
  LOB_CHECK_EQ(l.validation.checked, std::uint64_t{6});
  LOB_CHECK_EQ(l.validation.skipped, std::uint64_t{1});
  LOB_CHECK_EQ(l.validation.mismatched, std::uint64_t{0});
  LOB_CHECK_EQ(l.exit_code(), 0);

  auto rows = book_rows();
  rows[3] = "1000100,50,1000000,100,9999999999,0,999900";  // 15 fields
  const std::string bad_book = join(rows);
  const SessionResult b = session(kMsgs, &bad_book);
  LOB_CHECK(b.stopped_early);
  LOB_CHECK_EQ(b.book_parse_errors, std::uint64_t{1});
  LOB_CHECK_EQ(b.first_book_error.line, std::uint64_t{4});
  const SessionResult bl = session(kMsgs, &bad_book, opt);
  LOB_CHECK_EQ(bl.book_parse_errors, std::uint64_t{1});
  LOB_CHECK_EQ(bl.validation.skipped, std::uint64_t{1});
  LOB_CHECK_EQ(bl.validation.mismatched, std::uint64_t{0});
  // Row 5 (type 4) is compared with the engine only; row 6 (type 7) has row 5 as its previous row.
  LOB_CHECK_EQ(bl.validation.unchanged_checked, std::uint64_t{1});
}

LOB_TEST(session_crlf_trailing_commas_and_blank_lines) {
  std::string msgs;
  for (char c : std::string(kMsgs)) {
    if (c == '\n') {
      msgs += ",\r\n";
    } else {
      msgs += c;
    }
  }
  msgs += "\r\n";  // trailing blank line
  msgs.pop_back();
  std::string book;
  for (const auto& r : book_rows()) book += r + " ,\r\n";
  book.pop_back();
  book.pop_back();  // last line without a newline
  const SessionResult r = session(msgs, &book);
  LOB_CHECK(r.fatal.empty());
  LOB_CHECK_EQ(r.validation.checked, std::uint64_t{7});
  LOB_CHECK_EQ(r.validation.mismatched, std::uint64_t{0});
  LOB_CHECK_EQ(r.exit_code(), 0);
}

// ---- Stats --------------------------------------------------------------------------------------

LOB_TEST(stats_hand_computed) {
  // t=0 bid 100 @ 1000000; t=1 ask 50 @ 1000300; t=3 execute 20 of the ask; t=4 bid 10 @ 1000100;
  // t=8 halt. Intervals: [0,1) bid only; [1,3) spread 300, sizes 100/50; [3,4) spread 300,
  // 100/30; [4,8) spread 200, 10/30.
  const std::string msgs =
      "0,1,1,100,1000000,1\n"
      "1,1,2,50,1000300,-1\n"
      "3,4,2,20,1000300,-1\n"
      "4,1,3,10,1000100,1\n"
      "8,7,0,0,-1,0\n";
  SessionOptions opt;
  opt.stats = true;
  const SessionResult r = session(msgs, nullptr, opt);
  const StatsResult& s = r.stats;
  LOB_CHECK(r.has_stats);
  LOB_CHECK_EQ(s.messages, std::uint64_t{5});
  LOB_CHECK_EQ(s.by_type[1], std::uint64_t{3});
  LOB_CHECK_EQ(s.executed_ask, Qty{20});
  LOB_CHECK_EQ(s.executed_bid, Qty{0});
  LOB_CHECK_NEAR(s.two_sided_seconds, 7.0, 1e-12);
  LOB_CHECK_NEAR(s.mean_spread_ticks, 1700.0 / 7.0, 1e-9);
  LOB_CHECK_NEAR(s.mean_best_bid_qty, 440.0 / 8.0, 1e-9);
  LOB_CHECK_NEAR(s.mean_best_ask_qty, 250.0 / 7.0, 1e-9);
  LOB_CHECK_EQ(s.best_price_changes, std::uint64_t{3});
  LOB_CHECK_EQ(s.best_quote_changes, std::uint64_t{4});
  LOB_CHECK_NEAR(s.frac_best_price_changes(), 0.6, 1e-12);
}

LOB_TEST(stats_hidden_volume_by_direction) {
  const std::string msgs =
      "0,5,0,10,1000000,1\n"
      "0,5,0,7,1000000,-1\n"
      "0,5,0,3,1000000,0\n"
      "0,6,0,99,1000000,1\n";
  SessionOptions opt;
  opt.stats = true;
  const SessionResult r = session(msgs, nullptr, opt);
  LOB_CHECK_EQ(r.stats.hidden_buy, Qty{10});
  LOB_CHECK_EQ(r.stats.hidden_sell, Qty{7});
  LOB_CHECK_EQ(r.stats.hidden_other, Qty{3});
  LOB_CHECK_EQ(r.stats.two_sided_seconds, 0.0);
  LOB_CHECK_EQ(r.stats.mean_spread_ticks, 0.0);
  LOB_CHECK_EQ(r.stats.best_quote_changes, std::uint64_t{0});
}

LOB_TEST_MAIN()
