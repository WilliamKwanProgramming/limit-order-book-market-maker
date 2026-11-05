// Simulator tests: determinism, common random numbers across strategies, accounting identity,
// engine invariants during simulation, post-only and inventory-limit enforcement, order-manager
// behaviour (queue position kept, latency), markouts on a hand-built example, and fill rate
// against quote distance.
#include <cmath>
#include <cstring>
#include <memory>

#include "lob/backtest/experiment.hpp"
#include "lob/backtest/market.hpp"
#include "lob/check.hpp"

using namespace lob;
using namespace lob::backtest;

namespace {

SimParams small_params() {
  SimParams p;
  p.horizon = 120.0;
  p.burn_in = 60.0;
  p.markout_tail = 10.0;
  return p;
}

AsParams as_params(const SimParams& p, double gamma) {
  AsParams a;
  a.gamma = gamma;
  a.sigma = 0.67;
  a.k = 1.5;
  a.horizon = p.horizon;
  a.requote = p.requote;
  return a;
}

// Quotes at random offsets drawn from the strategy's own stream (to test stream separation).
class RandomQuoter final : public Strategy {
 public:
  QuoteTarget quote(const StrategyView& v) override {
    if (!v.mid_defined) return {};
    const double ob = 0.5 + 3.0 * v.rng->uniform();
    const double oa = 0.5 + 3.0 * v.rng->uniform();
    const bool wb = v.rng->bernoulli(0.8), wa = v.rng->bernoulli(0.8);
    return apply_quote_rules(v.mid - ob, v.mid + oa, wb, wa, v);
  }
};

// Ignores every rule: tries to cross the book on both sides.
class CrossingQuoter final : public Strategy {
 public:
  QuoteTarget quote(const StrategyView& v) override {
    QuoteTarget q;
    q.lots = 1;
    if (v.best_ask) {
      q.bid_on = true;
      q.bid = *v.best_ask + static_cast<Price>(cycle_ % 3);  // at or through the ask
    }
    if (v.best_bid) {
      q.ask_on = true;
      q.ask = *v.best_bid - static_cast<Price>(cycle_ % 3);
    }
    ++cycle_;
    return q;
  }

 private:
  unsigned cycle_ = 0;
};

// Always bids just below the ask (gets hit often) and never checks inventory; alternates the price
// by one tick so that orders are cancelled and resubmitted while others may still rest.
class GreedyBidder final : public Strategy {
 public:
  QuoteTarget quote(const StrategyView& v) override {
    QuoteTarget q;
    q.lots = 1;
    if (v.best_ask) {
      q.bid_on = true;
      q.bid = *v.best_ask - 1 - static_cast<Price>(cycle_++ % 2);
    }
    return q;
  }

 private:
  unsigned cycle_ = 0;
};

// Constant prices far from the market.
class FarQuoter final : public Strategy {
 public:
  QuoteTarget quote(const StrategyView&) override {
    QuoteTarget q;
    q.lots = 1;
    q.bid_on = q.ask_on = true;
    q.bid = 9000;
    q.ask = 11000;
    return q;
  }
};

std::unique_ptr<Strategy> make(int which, const SimParams& p) {
  switch (which) {
    case 0: return std::make_unique<NeverQuote>();
    case 1: return std::make_unique<NaiveTouch>();
    case 2: return std::make_unique<AvellanedaStoikov>(as_params(p, 0.003));
    case 3: return std::make_unique<SymmetricMatched>(as_params(p, 0.003));
    case 4: return std::make_unique<RandomQuoter>();
    case 5: return std::make_unique<CrossingQuoter>();
    case 6: return std::make_unique<GreedyBidder>();
    default: return std::make_unique<FarQuoter>();
  }
}
constexpr int kStrategies = 8;

bool same_bytes(const EpisodeResult& a, const EpisodeResult& b) {
  // Field-by-field (the struct has padding, so memcmp of the whole object is not meaningful).
  bool ok = a.pnl == b.pnl && a.final_inventory == b.final_inventory &&
            a.abs_inventory_time == b.abs_inventory_time && a.fills == b.fills &&
            a.fill_lots == b.fill_lots && a.quote_seconds == b.quote_seconds &&
            a.quote_dist_time == b.quote_dist_time && a.spread_captured == b.spread_captured &&
            a.submits == b.submits && a.cancels == b.cancels &&
            a.post_only_rejects == b.post_only_rejects && a.limit_rejects == b.limit_rejects &&
            a.spread_time == b.spread_time && a.touch_depth_time == b.touch_depth_time &&
            a.market_orders == b.market_orders && a.informed_orders == b.informed_orders &&
            a.mid_start == b.mid_start && a.mid_end == b.mid_end &&
            a.vol_ss == b.vol_ss && a.reach_hist == b.reach_hist && a.markout_sum == b.markout_sum;
  ok = ok && (a.fill_dispersion == b.fill_dispersion ||
              (std::isnan(a.fill_dispersion) && std::isnan(b.fill_dispersion)));
  return ok;
}

EpisodeResult run(const SimParams& p, std::uint64_t seed, int which, EpisodeLog* log = nullptr) {
  Market m(p, seed);
  m.burn_in();
  auto s = make(which, p);
  return m.run_episode(*s, log);
}

}  // namespace

LOB_TEST(same_seed_same_result) {
  SimParams p = small_params();
  p.news_rate = 0.05;
  p.branching = 0.7;
  for (int which = 0; which < kStrategies; ++which) {
    const EpisodeResult a = run(p, 77, which);
    const EpisodeResult b = run(p, 77, which);
    LOB_CHECK(same_bytes(a, b));
  }
  // A copy of a burned-in market gives the same result as a freshly burned-in one.
  Market base(p, 78);
  base.burn_in();
  Market copy = base;
  auto s1 = make(2, p);
  auto s2 = make(2, p);
  LOB_CHECK(same_bytes(copy.run_episode(*s1), run(p, 78, 2)));
  (void)s2;
  // run_cell is deterministic too (CSV-level).
  std::vector<StrategySpec> specs;
  std::string err;
  LOB_CHECK(parse_strategies("naive,as:0.003,sym:0.003", specs, err));
  const CellResults r1 = run_cell(p, specs, {0.67, 1.5}, 5, 3);
  const CellResults r2 = run_cell(p, specs, {0.67, 1.5}, 5, 3);
  for (std::size_t s = 0; s < specs.size(); ++s)
    for (std::size_t e = 0; e < 3; ++e) LOB_CHECK(same_bytes(r1[s][e], r2[s][e]));
}

LOB_TEST(different_seeds_differ) {
  const SimParams p = small_params();
  const EpisodeResult a = run(p, 1, 1), b = run(p, 2, 1);
  LOB_CHECK(!same_bytes(a, b));
}

LOB_TEST(exogenous_events_identical_across_strategies) {
  for (double nu : {0.0, 0.08}) {
    SimParams p = small_params();
    p.news_rate = nu;
    p.branching = 0.8;
    for (std::uint64_t seed : {11ULL, 12ULL}) {
      EpisodeLog ref;
      run(p, seed, 0, &ref);
      LOB_CHECK(ref.exo.size() > 1000);
      std::size_t informed = 0;
      for (const ExoRecord& r : ref.exo) informed += r.kind == 3 ? 1 : 0;
      LOB_CHECK(nu == 0.0 ? informed == 0 : informed > 0);
      for (int which = 1; which < kStrategies; ++which) {
        EpisodeLog log;
        const EpisodeResult r = run(p, seed, which, &log);
        LOB_CHECK(log.exo == ref.exo);
        if (which == 1 || which == 4) LOB_CHECK(r.fills > 0);  // the strategy did interact
      }
    }
  }
}

LOB_TEST(accounting_identity_from_raw_fills) {
  SimParams p = small_params();
  p.news_rate = 0.05;
  for (int which = 1; which < kStrategies; ++which) {
    for (std::uint64_t seed = 20; seed < 23; ++seed) {
      EpisodeLog log;
      const EpisodeResult r = run(p, seed, which, &log);
      // Recompute cash and inventory from the engine's fill records alone.
      std::int64_t cash = 0;  // tick-shares
      Qty shares = 0;
      for (const Fill& f : log.raw_fills) {
        LOB_CHECK(is_own_id(f.resting_id));
        if (f.resting_side == Side::Bid) {
          shares += f.qty;
          cash -= f.price * f.qty;
        } else {
          shares -= f.qty;
          cash += f.price * f.qty;
        }
      }
      LOB_CHECK_EQ(log.raw_fills.size(), log.fills.size());
      LOB_CHECK_EQ(static_cast<std::int64_t>(log.raw_fills.size()), r.fills);
      LOB_CHECK_EQ(shares % kSharesPerLot, Qty{0});
      LOB_CHECK_EQ(shares / kSharesPerLot, r.final_inventory);
      LOB_CHECK_EQ(cash, log.final_cash);
      // Mark at the others' mid at T, read back from the recorded mid path.
      const Time t_end = seconds_to_ns(p.burn_in + p.horizon);
      const double mid_T = mid_at(log.mid_path, t_end);
      LOB_CHECK_EQ(mid_T, log.final_mid);
      const double pnl = 0.01 * static_cast<double>(cash) + 0.01 * static_cast<double>(shares) * mid_T;
      LOB_CHECK_NEAR(pnl, r.pnl, 1e-6);
      // Fills are all inside the episode.
      for (const OwnFill& f : log.fills) {
        LOB_CHECK(f.t >= seconds_to_ns(p.burn_in) && f.t <= t_end);
      }
    }
  }
}

LOB_TEST(never_quote_has_zero_pnl_and_fills) {
  SimParams p = small_params();
  p.news_rate = 0.1;
  for (std::uint64_t seed = 30; seed < 35; ++seed) {
    EpisodeLog log;
    const EpisodeResult r = run(p, seed, 0, &log);
    LOB_CHECK_EQ(r.pnl, 0.0);
    LOB_CHECK_EQ(r.fills, std::int64_t{0});
    LOB_CHECK_EQ(r.final_inventory, Qty{0});
    LOB_CHECK_EQ(r.submits, std::int64_t{0});
    LOB_CHECK_EQ(r.quote_seconds, 0.0);
    LOB_CHECK(log.submits.empty());
    LOB_CHECK(std::isnan(r.fill_dispersion));
  }
}

LOB_TEST(engine_invariants_hold_during_simulation) {
  SimParams p = small_params();
  p.check_every = 13;
  p.news_rate = 0.1;
  p.branching = 0.9;
  for (int which = 0; which < kStrategies; ++which) {
    EpisodeLog log;
    const EpisodeResult r = run(p, 40 + static_cast<std::uint64_t>(which), which, &log);
    LOB_CHECK(log.invariant_checks > 100);
    LOB_CHECK_EQ(r.invariant_failures, std::int64_t{0});
    if (!log.first_invariant_failure.empty())
      std::fprintf(stderr, "  invariant: %s\n", log.first_invariant_failure.c_str());
  }
}

LOB_TEST(post_only_and_inventory_limit_are_enforced) {
  SimParams p = small_params();
  p.max_inventory = 3;
  p.news_rate = 0.05;
  p.latency = 0.02;  // long enough for the market to move between decision and effect
  for (int which = 1; which < kStrategies; ++which) {
    for (std::uint64_t seed = 50; seed < 53; ++seed) {
      EpisodeLog log;
      const EpisodeResult r = run(p, seed, which, &log);
      // Our orders never take liquidity: every fill on our ids has us resting and a market order
      // as aggressor.
      for (const Fill& f : log.raw_fills) {
        LOB_CHECK(is_own_id(f.resting_id));
        LOB_CHECK(f.aggressor_id >= kMarketIdBase && f.aggressor_id < kOwnIdBase);
      }
      // Every accepted submission was strictly behind the opposite best and within the limit.
      std::int64_t accepted = 0;
      for (const SubmitRecord& s : log.submits) {
        if (!s.accepted) continue;
        ++accepted;
        if (s.opposite_best) {
          LOB_CHECK(s.side == Side::Bid ? s.price < *s.opposite_best : s.price > *s.opposite_best);
        }
        const Qty exposure = (s.side == Side::Bid ? s.inventory : -s.inventory) +
                             s.resting_same_side + 1;
        LOB_CHECK(exposure <= p.max_inventory);
      }
      LOB_CHECK_EQ(accepted, r.submits);
      for (const auto& ti : log.inventory_path) LOB_CHECK(std::abs(ti.second) <= p.max_inventory);
      LOB_CHECK(r.max_abs_inventory <= p.max_inventory);
      if (which == 5) {
        // Most crossing targets are rejected; the few accepted ones had become non-crossing
        // because the market moved during the latency (checked above).
        LOB_CHECK(r.post_only_rejects > 100);
        LOB_CHECK(r.post_only_rejects > 10 * r.submits);
      }
      if (which == 6) {
        LOB_CHECK_EQ(r.max_abs_inventory, p.max_inventory);  // reaches the limit...
        LOB_CHECK(r.limit_rejects > 0);                     // ...and is stopped there
      }
    }
  }
}

LOB_TEST(unchanged_quotes_keep_queue_position_and_latency_applies) {
  SimParams p = small_params();
  p.latency = 0.0037;
  EpisodeLog log;
  const EpisodeResult r = run(p, 60, 7, &log);
  // FarQuoter never changes its target: one submit per side, no cancels until the end.
  LOB_CHECK_EQ(r.submits, std::int64_t{2});
  LOB_CHECK_EQ(r.cancels, std::int64_t{2});  // the two end-of-episode cancels
  LOB_CHECK_EQ(log.submits.size(), std::size_t{2});
  const Time expect = seconds_to_ns(p.burn_in) + seconds_to_ns(p.latency);
  for (const SubmitRecord& s : log.submits) LOB_CHECK_EQ(s.t, expect);
  // Quote time is the whole episode minus the latency, on both sides.
  LOB_CHECK_NEAR(r.quote_seconds, 2.0 * (p.horizon - p.latency), 1e-9);
  // Latency for a strategy that moves: every submission lands latency after a requote time.
  EpisodeLog log2;
  run(p, 61, 1, &log2);
  LOB_CHECK(log2.submits.size() > 10);
  for (const SubmitRecord& s : log2.submits) {
    const Time since = s.t - seconds_to_ns(p.burn_in) - seconds_to_ns(p.latency);
    const Time step = seconds_to_ns(p.requote);
    LOB_CHECK(since >= 0);
    LOB_CHECK(std::llabs(since - (since + step / 2) / step * step) <= 1);
  }
}

LOB_TEST(markouts_on_a_hand_built_example) {
  const Time s = kNanosPerSecond;
  // Mid path: 100 until t = 1 s, 102 from 1 s, 99 from 5 s, 104 from 20 s.
  const std::vector<MidPoint> path = {{0, 100.0}, {1 * s, 102.0}, {5 * s, 99.0}, {20 * s, 104.0}};
  LOB_CHECK_EQ(mid_at(path, 0), 100.0);
  LOB_CHECK_EQ(mid_at(path, s - 1), 100.0);
  LOB_CHECK_EQ(mid_at(path, s), 102.0);
  LOB_CHECK_EQ(mid_at(path, 7 * s), 99.0);
  LOB_CHECK_EQ(mid_at(path, 100 * s), 104.0);
  const std::vector<OwnFill> fills = {
      // Buy 1 lot at 99 at t = 0.5 s: mid 100 at 0.6 s, 102 at 1.5 s, 99 at 10.5 s.
      {s / 2, Side::Bid, 99, 1, 100.0},
      // Sell 2 lots at 103 at t = 12 s: mid 99 at 12.1 s and 13 s, 104 at 22 s.
      {12 * s, Side::Ask, 103, 2, 99.0},
  };
  double sums[3] = {0, 0, 0};
  add_markouts(fills, path, kMarkoutHorizons.data(), 3, sums);
  // Buy: (100 - 99), (102 - 99), (99 - 99). Sell: 2 * (103 - 99), 2 * (103 - 99), 2 * (103 - 104).
  LOB_CHECK_NEAR(sums[0], 1.0 + 8.0, 1e-12);
  LOB_CHECK_NEAR(sums[1], 3.0 + 8.0, 1e-12);
  LOB_CHECK_NEAR(sums[2], 0.0 - 2.0, 1e-12);
  // A buy followed by a price drop is a negative markout.
  double one[3] = {0, 0, 0};
  const std::vector<OwnFill> loser = {{6 * s, Side::Bid, 100, 1, 99.5}};
  add_markouts(loser, path, kMarkoutHorizons.data(), 3, one);
  LOB_CHECK_NEAR(one[0], -1.0, 1e-12);
  LOB_CHECK_NEAR(one[2], -1.0, 1e-12);  // 16 s: mid 99
}

LOB_TEST(dispersion_index_hand_values) {
  LOB_CHECK_NEAR(dispersion_index({1, 1, 1, 1}), 0.0, 1e-15);
  // counts 0, 2, 4: mean 2, sample variance 4 -> 2.
  LOB_CHECK_NEAR(dispersion_index({0, 2, 4}), 2.0, 1e-15);
  LOB_CHECK(std::isnan(dispersion_index({0, 0, 0})));
  LOB_CHECK(std::isnan(dispersion_index({3})));
  // Uninformed market orders are Poisson: dispersion near 1 (diagnostic of the generator).
  const SimParams p = small_params();
  std::vector<StrategySpec> specs;
  std::string err;
  parse_strategies("none", specs, err);
  const CellResults r = run_cell(p, specs, {1.0, 1.0}, 3, 20);
  const MarketDiagnostics d = diagnose(r[0], p.horizon);
  // 20 x 120 bins: SE of the mean dispersion is about sqrt(2 / 2400) = 0.03.
  LOB_CHECK_NEAR(d.mo_dispersion, 1.0, 0.15);
  LOB_CHECK_NEAR(d.mo_rate, p.mo_rate, 0.1);
}

LOB_TEST(fill_rate_decreases_with_quote_distance) {
  SimParams p = small_params();
  p.horizon = 300.0;
  p.max_inventory = 1000;  // keep the probes quoting on both sides
  std::vector<StrategySpec> specs;
  std::string err;
  LOB_CHECK(parse_strategies("fixed:0.5,fixed:1.5,fixed:2.5,fixed:3.5", specs, err));
  const CellResults r = run_cell(p, specs, {1.0, 1.0}, 9, 12);
  double prev_rate = 1e9, prev_dist = -1;
  for (std::size_t s = 0; s < specs.size(); ++s) {
    const StrategySummary sm = summarize(r[s], p.horizon);
    std::printf("  offset %s: distance %.3f ticks, fill rate %.5f /quote-s, fills %.1f\n",
                specs[s].label.c_str(), sm.quote_dist, sm.fill_rate, sm.mean_fills);
    LOB_CHECK(sm.quote_dist > prev_dist);
    LOB_CHECK(sm.fill_rate < prev_rate);
    LOB_CHECK(sm.mean_fills > 0);
    prev_rate = sm.fill_rate;
    prev_dist = sm.quote_dist;
  }
}

LOB_TEST(own_quotes_do_not_move_the_others_mid) {
  // Our own orders are excluded from the reference mid: a strategy that improves the touch does
  // not change the mid it is measured against until the book itself changes. With no own fills,
  // the others' mid path is identical with and without our resting orders far away.
  const SimParams p = small_params();
  EpisodeLog a, b;
  run(p, 70, 0, &a);
  run(p, 70, 7, &b);  // FarQuoter: rests at 9000/11000, never trades
  LOB_CHECK_EQ(a.mid_path.size(), b.mid_path.size());
  bool same = a.mid_path.size() == b.mid_path.size();
  for (std::size_t i = 0; same && i < a.mid_path.size(); ++i)
    same = a.mid_path[i].t == b.mid_path[i].t && a.mid_path[i].mid == b.mid_path[i].mid;
  LOB_CHECK(same);
}

LOB_TEST_MAIN()
