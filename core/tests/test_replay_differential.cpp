// Randomized differential test of replay mode: lob::OrderBook against the naive ReferenceBook.
//
// Replay mode never matches, so bids and asks are drawn from overlapping price ranges and the book
// is frequently locked or crossed; executions hit any resting order (not only the touch) with
// sizes that sometimes exceed what remains. Return codes, fills and full depth must agree after
// every operation; check_invariants(false) runs every 1000 operations.
#include <cstdio>
#include <string>
#include <vector>

#include "lob/check.hpp"
#include "lob/order_book.hpp"
#include "lob/rng.hpp"
#include "reference_book.hpp"

using namespace lob;
using lob::testing::ReferenceBook;

namespace {

constexpr long kOpsPerSeed = 550'000;
constexpr std::uint64_t kSeeds[] = {11, 12, 13, 20261104};

struct Coverage {
  long ops = 0, adds = 0, cancels = 0, reduces = 0, executions = 0, rejects = 0, crossed = 0;
  double sum_orders = 0;
};

OrderId pick_target(Rng& rng, const std::vector<OrderId>& live, OrderId next_id) {
  const std::uint64_t u = rng.uniform_int(100);
  if (u == 0) return kNoOrder;
  if (u < 4 || live.empty()) {
    return static_cast<OrderId>(rng.uniform_int(1, static_cast<std::int64_t>(next_id) + 5));
  }
  return live[rng.uniform_int(live.size())];
}

bool run_seed(std::uint64_t seed, Coverage& cov) {
  Rng rng(seed);
  OrderBook book;
  ReferenceBook ref;
  std::vector<Fill> got;
  std::vector<Fill> want;
  std::vector<OrderId> live;
  OrderId next_id = 1;
  Price center = 5'000'000;

  for (long i = 0; i < kOpsPerSeed; ++i) {
    got.clear();
    want.clear();
    if (rng.uniform_int(500) == 0) center += rng.uniform_int(-3, 3);
    const Time ts = i;
    ref.ids(live);
    cov.sum_orders += static_cast<double>(live.size());
    const std::uint64_t bias = live.size() > 300 ? 15 : 0;
    const std::uint64_t u = rng.uniform_int(100);

    char what[160];
    Status s_got;
    Status s_want;
    OrderId touched = kNoOrder;

    if (u < 52 - bias) {  // add: both sides share one price band, so crossing is common
      const Side side = rng.bernoulli(0.5) ? Side::Bid : Side::Ask;
      OrderId id = next_id++;
      if (rng.uniform_int(100) == 0 && !live.empty()) id = live[rng.uniform_int(live.size())];
      Price price = center + rng.uniform_int(-15, 15);
      if (rng.uniform_int(300) == 0) price = -rng.uniform_int(0, 2);
      Qty qty = rng.uniform_int(1, 500);
      if (rng.uniform_int(300) == 0) qty = -rng.uniform_int(0, 2);
      ++cov.adds;
      std::snprintf(what, sizeof what, "add_resting(%llu, %d, %lld, %lld)",
                    static_cast<unsigned long long>(id), static_cast<int>(side),
                    static_cast<long long>(price), static_cast<long long>(qty));
      s_got = book.add_resting(id, side, price, qty, ts);
      s_want = ref.add_resting(id, side, price, qty, ts);
      touched = id;
    } else if (u < 82) {  // full deletion
      const OrderId id = pick_target(rng, live, next_id);
      ++cov.cancels;
      std::snprintf(what, sizeof what, "cancel(%llu)", static_cast<unsigned long long>(id));
      s_got = book.cancel(id);
      s_want = ref.cancel(id);
      touched = id;
    } else if (u < 90) {  // partial cancellation
      const OrderId id = pick_target(rng, live, next_id);
      const Qty by = rng.bernoulli(0.02) ? 0 : rng.uniform_int(1, 300);
      ++cov.reduces;
      std::snprintf(what, sizeof what, "reduce(%llu, %lld)", static_cast<unsigned long long>(id),
                    static_cast<long long>(by));
      s_got = book.reduce(id, by);
      s_want = ref.reduce(id, by);
      touched = id;
    } else {  // execution of a resting order, usually the front of the best level
      OrderId id = pick_target(rng, live, next_id);
      const Side side = rng.bernoulli(0.5) ? Side::Bid : Side::Ask;
      if (rng.bernoulli(0.6) && book.front(side)) id = book.front(side)->id;
      const Qty qty = rng.bernoulli(0.02) ? 0 : rng.uniform_int(1, 400);
      ++cov.executions;
      std::snprintf(what, sizeof what, "execute_resting(%llu, %lld)",
                    static_cast<unsigned long long>(id), static_cast<long long>(qty));
      s_got = book.execute_resting(id, qty, ts, got);
      s_want = ref.execute_resting(id, qty, ts, want);
      touched = id;
    }
    ++cov.ops;
    if (s_want != Status::Ok) ++cov.rejects;
    if (book.spread() && *book.spread() <= 0) ++cov.crossed;

    std::string problem;
    if (s_got != s_want) {
      problem = std::string("status ") + to_string(s_got) + " vs " + to_string(s_want);
    } else {
      problem = lob::testing::diff_fills(got, want);
    }
    if (problem.empty()) problem = lob::testing::diff_books(book, ref);
    if (problem.empty() && touched != kNoOrder) {
      const auto a = book.find(touched);
      const auto b = ref.find(touched);
      if (a.has_value() != b.has_value() ||
          (a && (a->side != b->side || a->price != b->price || a->qty != b->qty ||
                 a->ts != b->ts))) {
        problem = "find() differs";
      } else if (book.queue_ahead(touched) != ref.queue_ahead(touched)) {
        problem = "queue_ahead() differs";
      }
    }
    std::string why;
    if (problem.empty() && i % 1000 == 999 && !book.check_invariants(false, &why)) {
      problem = "check_invariants: " + why;
    }
    if (!problem.empty()) {
      std::fprintf(stderr, "  seed %llu op %ld %s: %s\n", static_cast<unsigned long long>(seed),
                   i, what, problem.c_str());
      LOB_CHECK(problem.empty());
      return false;
    }
  }
  std::string why;
  LOB_CHECK(book.check_invariants(false, &why));
  return true;
}

}  // namespace

LOB_TEST(replay_mode_matches_reference_model) {
  Coverage cov;
  for (const std::uint64_t seed : kSeeds) {
    if (!run_seed(seed, cov)) return;
  }
  LOB_CHECK(cov.ops >= 2'000'000);
  LOB_CHECK(cov.crossed > 0);  // the test must actually exercise locked/crossed books
  std::printf("  %ld ops over %zu seeds: %ld add, %ld cancel, %ld reduce, %ld execute, "
              "%ld rejected; locked/crossed after %ld ops; resting orders mean %.0f\n",
              cov.ops, sizeof(kSeeds) / sizeof(kSeeds[0]), cov.adds, cov.cancels, cov.reduces,
              cov.executions, cov.rejects, cov.crossed,
              cov.sum_orders / static_cast<double>(cov.ops));
}

LOB_TEST_MAIN()
