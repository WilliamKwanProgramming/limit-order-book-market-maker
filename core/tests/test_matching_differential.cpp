// Randomized differential test of matching mode: lob::OrderBook against the naive ReferenceBook.
//
// Every operation is applied to both books; the return values, every fill, and the full depth of
// both sides must agree after every operation, and check_invariants() runs every 1000 operations.
// The order flow is deliberately rough: prices straddle the touch so many limits cross, sizes
// sometimes sweep several levels, and a small fraction of calls use invalid, duplicate or unknown
// ids so the error paths are compared too.
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

constexpr long kOpsPerSeed = 600'000;
constexpr std::uint64_t kSeeds[] = {1, 2, 3, 20261104};

struct Coverage {
  long ops = 0, fills = 0, crossing_limits = 0, market = 0, cancels = 0, reduces = 0,
       replaces = 0, rejects = 0;
  std::size_t max_orders = 0;
  double sum_orders = 0;
};

Side random_side(Rng& rng) { return rng.bernoulli(0.5) ? Side::Bid : Side::Ask; }

// An id for cancel/reduce/replace: usually a resting order, sometimes unknown or invalid.
OrderId pick_target(Rng& rng, const std::vector<OrderId>& live, OrderId next_id) {
  const std::uint64_t u = rng.uniform_int(100);
  if (u == 0) return kNoOrder;
  if (u < 4 || live.empty()) return static_cast<OrderId>(rng.uniform_int(1, static_cast<std::int64_t>(next_id) + 5));
  return live[rng.uniform_int(live.size())];
}

// A size that is occasionally invalid and occasionally large enough to sweep levels.
Qty pick_qty(Rng& rng) {
  const std::uint64_t u = rng.uniform_int(200);
  if (u == 0) return 0;
  if (u == 1) return -static_cast<Qty>(rng.uniform_int(1, 10));
  if (u < 12) return rng.uniform_int(100, 1500);
  return rng.uniform_int(1, 100);
}

bool run_seed(std::uint64_t seed, Coverage& cov) {
  Rng rng(seed);
  OrderBook book;
  ReferenceBook ref;
  std::vector<Fill> got;
  std::vector<Fill> want;
  std::vector<OrderId> live;
  OrderId next_id = 1;
  Price center = 10'000;

  for (long i = 0; i < kOpsPerSeed; ++i) {
    got.clear();
    want.clear();
    if (rng.uniform_int(500) == 0) center += rng.uniform_int(-3, 3);
    const Time ts = i;
    ref.ids(live);
    cov.max_orders = std::max(cov.max_orders, live.size());
    cov.sum_orders += static_cast<double>(live.size());
    // Shift weight between adds and cancels so the book hovers around a few hundred orders.
    const std::uint64_t bias = live.size() > 300 ? 20 : 0;
    const std::uint64_t u = rng.uniform_int(100);

    char what[160];
    SubmitResult r_got;
    SubmitResult r_want;
    OrderId touched = kNoOrder;

    if (u < 58 - bias) {  // limit order, often crossing
      const Side side = random_side(rng);
      OrderId id = next_id++;
      if (rng.uniform_int(100) == 0 && !live.empty()) id = live[rng.uniform_int(live.size())];
      // Offset below zero means the order is priced through the mid and will usually cross.
      const Price offset = rng.uniform_int(-4, 20);
      Price price = side == Side::Bid ? center - offset : center + offset;
      if (rng.uniform_int(300) == 0) price = -rng.uniform_int(0, 2);
      const Qty qty = pick_qty(rng);
      if (offset < 0) ++cov.crossing_limits;
      std::snprintf(what, sizeof what, "submit_limit(%llu, %d, %lld, %lld)",
                    static_cast<unsigned long long>(id), static_cast<int>(side),
                    static_cast<long long>(price), static_cast<long long>(qty));
      r_got = book.submit_limit(id, side, price, qty, ts, got);
      r_want = ref.submit_limit(id, side, price, qty, ts, want);
      touched = id;
    } else if (u < 63 - bias) {  // market order
      const Side side = random_side(rng);
      const OrderId id = next_id++;
      const Qty qty = pick_qty(rng);
      ++cov.market;
      std::snprintf(what, sizeof what, "submit_market(%llu, %d, %lld)",
                    static_cast<unsigned long long>(id), static_cast<int>(side),
                    static_cast<long long>(qty));
      r_got = book.submit_market(id, side, qty, ts, got);
      r_want = ref.submit_market(id, side, qty, ts, want);
    } else if (u < 83) {  // cancel
      const OrderId id = pick_target(rng, live, next_id);
      ++cov.cancels;
      std::snprintf(what, sizeof what, "cancel(%llu)", static_cast<unsigned long long>(id));
      r_got.status = book.cancel(id);
      r_want.status = ref.cancel(id);
      touched = id;
    } else if (u < 90) {  // reduce
      const OrderId id = pick_target(rng, live, next_id);
      const Qty by = rng.bernoulli(0.02) ? 0 : rng.uniform_int(1, 60);
      ++cov.reduces;
      std::snprintf(what, sizeof what, "reduce(%llu, %lld)", static_cast<unsigned long long>(id),
                    static_cast<long long>(by));
      r_got.status = book.reduce(id, by);
      r_want.status = ref.reduce(id, by);
      touched = id;
    } else {  // replace: keep price half the time, move it (possibly through the touch) otherwise
      const OrderId id = pick_target(rng, live, next_id);
      const auto info = ref.find(id);
      Price price = info ? info->price : center;
      Qty qty = info ? info->qty : 10;
      if (rng.bernoulli(0.5)) price += rng.uniform_int(-8, 8);
      qty += rng.uniform_int(-qty, 20);  // smaller, equal or larger; sometimes zero
      ++cov.replaces;
      std::snprintf(what, sizeof what, "replace(%llu, %lld, %lld)",
                    static_cast<unsigned long long>(id), static_cast<long long>(price),
                    static_cast<long long>(qty));
      r_got = book.replace(id, price, qty, ts, got);
      r_want = ref.replace(id, price, qty, ts, want);
      touched = id;
    }
    ++cov.ops;
    cov.fills += static_cast<long>(want.size());
    if (r_want.status != Status::Ok) ++cov.rejects;

    std::string problem;
    if (r_got.status != r_want.status) {
      problem = std::string("status ") + to_string(r_got.status) + " vs " + to_string(r_want.status);
    } else if (r_got.filled != r_want.filled || r_got.resting != r_want.resting) {
      problem = "result filled/resting " + std::to_string(r_got.filled) + "/" +
                std::to_string(r_got.resting) + " vs " + std::to_string(r_want.filled) + "/" +
                std::to_string(r_want.resting);
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
    if (problem.empty() && i % 1000 == 999 && !book.check_invariants(true, &why)) {
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
  LOB_CHECK(book.check_invariants(true, &why));
  return true;
}

}  // namespace

LOB_TEST(matching_mode_matches_reference_model) {
  Coverage cov;
  for (const std::uint64_t seed : kSeeds) {
    if (!run_seed(seed, cov)) return;  // the first divergence is the useful one
  }
  LOB_CHECK(cov.ops >= 2'000'000);
  std::printf("  %ld ops over %zu seeds: %ld fills, %ld crossing limits, %ld market, %ld cancel, "
              "%ld reduce, %ld replace, %ld rejected; resting orders mean %.0f, max %zu\n",
              cov.ops, sizeof(kSeeds) / sizeof(kSeeds[0]), cov.fills, cov.crossing_limits,
              cov.market, cov.cancels, cov.reduces, cov.replaces, cov.rejects,
              cov.sum_orders / static_cast<double>(cov.ops), cov.max_orders);
}

LOB_TEST_MAIN()
