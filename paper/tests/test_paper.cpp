#include <cmath>
#include <vector>

#include "lob/avellaneda_stoikov.hpp"
#include "lob/check.hpp"
#include "lob/paper_experiment.hpp"

using namespace lob::paper;

namespace {

// Path with hand-chosen mids and fill uniforms. A uniform of 0 fills whenever P(fill) > 0;
// 0.5 does not fill when P(fill) < 0.5.
Path hand_path(std::vector<double> mid, std::vector<double> u_bid, std::vector<double> u_ask) {
  return Path{std::move(mid), std::move(u_bid), std::move(u_ask)};
}

// Three steps, low intensity, so that a quote one dollar away fills with probability
// A exp(-k) dt = exp(-1.5) / 3 ~ 0.074 < 0.5.
ModelParams tiny_model() {
  ModelParams p;
  p.A = 1.0;
  p.steps = 3;
  return p;
}

auto fixed_quotes(double delta_bid, double delta_ask) {
  return [=](int, double, double s, lob::Qty) { return Quotes{s - delta_bid, s + delta_ask}; };
}

}  // namespace

LOB_TEST(reservation_price_matches_hand_values) {
  const ModelParams p;  // sigma = 2, T = 1
  // 100 - 3 * 0.1 * 4 * 0.75 = 99.1
  LOB_CHECK_NEAR(reservation_price(100.0, 3.0, 0.25, 0.1, p), 99.1, 1e-12);
  // Short inventory raises the reservation price: 100 + 2 * 1 * 4 * 0.5 = 104
  LOB_CHECK_NEAR(reservation_price(100.0, -2.0, 0.5, 1.0, p), 104.0, 1e-12);
  // At the horizon the inventory term vanishes.
  LOB_CHECK_NEAR(reservation_price(100.0, 7.0, 1.0, 0.5, p), 100.0, 1e-12);
}

LOB_TEST(spread_matches_hand_values) {
  const ModelParams p;  // sigma = 2, T = 1, k = 1.5
  // 0.1 * 4 * 0.75 + 20 ln(16 / 15) = 0.3 + 1.290770422751424
  LOB_CHECK_NEAR(optimal_spread(0.25, 0.1, p), 1.590770422751424, 1e-12);
  // 1 * 4 * 1 + 2 ln(5 / 3) = 4 + 1.0216512475319814
  LOB_CHECK_NEAR(optimal_spread(0.0, 1.0, p), 5.0216512475319814, 1e-12);
  // Grid average: mean of (T - t_n) over n = 0..199 is 1 - 0.005 * 199 / 2 = 0.5025,
  // so 0.1 * 4 * 0.5025 + 1.290770422751424 = 1.491770422751424 (the paper rounds to 1.49).
  LOB_CHECK_NEAR(average_optimal_spread(0.1, p), 1.491770422751424, 1e-12);
}

LOB_TEST(small_gamma_limit) {
  const ModelParams p;
  for (double gamma : {1e-3, 1e-6, 1e-9}) {
    // r -> s, and the spread -> 2 / k (the risk-neutral limit).
    LOB_CHECK_NEAR(reservation_price(100.0, 10.0, 0.0, gamma, p), 100.0, 50.0 * gamma);
    LOB_CHECK_NEAR(optimal_spread(0.0, gamma, p), 2.0 / p.k, 10.0 * gamma);
  }
}

LOB_TEST(fill_probability_formula_and_cap) {
  const ModelParams p;  // A dt = 140 * 0.005 = 0.7
  LOB_CHECK_NEAR(fill_probability(0.5, p), 0.7 * std::exp(-0.75), 1e-15);
  LOB_CHECK_NEAR(fill_probability(0.0, p), 0.7, 1e-15);
  // A quote 0.1 through the mid: 0.7 exp(0.15) < 1, not yet capped.
  LOB_CHECK_NEAR(fill_probability(-0.1, p), 0.7 * std::exp(0.15), 1e-15);
  // Capped at 1 from delta = ln(0.7) / 1.5 ~ -0.238 down.
  LOB_CHECK_EQ(fill_probability(-0.3, p), 1.0);
  LOB_CHECK_EQ(fill_probability(-50.0, p), 1.0);
}

LOB_TEST(empirical_fill_frequency_matches_intensity) {
  const ModelParams p;
  struct Case {
    double delta_bid, delta_ask;
  };
  const Case cases[] = {{0.5, 2.0}, {0.0, 1.0}, {-0.1, 0.745}};
  Path path;
  for (const Case& c : cases) {
    long bids = 0, asks = 0, trials = 0;
    for (std::uint64_t i = 0; i < 1000; ++i) {
      generate_path(12345, i, p, path);
      const SimResult r = run_path(path, p, fixed_quotes(c.delta_bid, c.delta_ask));
      bids += r.bid_fills;
      asks += r.ask_fills;
      trials += p.steps;
    }
    // 200,000 Bernoulli trials per side; allow 5 binomial standard errors.
    for (const auto& [count, delta] : {std::pair<long, double>{bids, c.delta_bid},
                                       std::pair<long, double>{asks, c.delta_ask}}) {
      const double prob = p.A * std::exp(-p.k * delta) * p.dt();
      const double se = std::sqrt(prob * (1.0 - prob) / static_cast<double>(trials));
      LOB_CHECK_NEAR(static_cast<double>(count) / static_cast<double>(trials), prob, 5.0 * se);
    }
  }
  // Deep through the mid: certain fill on every step.
  generate_path(7, 0, p, path);
  const SimResult r = run_path(path, p, fixed_quotes(-1.0, -1.0));
  LOB_CHECK_EQ(r.bid_fills, p.steps);
  LOB_CHECK_EQ(r.ask_fills, p.steps);
}

LOB_TEST(profit_accounting_on_hand_path) {
  const ModelParams p = tiny_model();
  // Quotes s -/+ 1. Step 0 (s = 100): bid fills at 99. Step 1 (s = 101): ask fills at 102.
  // Step 2 (s = 99): bid fills at 98. Final mid 102.
  // Cash = -99 + 102 - 98 = -95, q = 1, profit = -95 + 102 = 7 (= +3 round trip, +4 on the
  // unit bought at 98 and marked at 102).
  const Path a = hand_path({100, 101, 99, 102}, {0.0, 0.5, 0.0}, {0.5, 0.0, 0.5});
  const SimResult ra = run_path(a, p, fixed_quotes(1.0, 1.0));
  LOB_CHECK_NEAR(ra.cash, -95.0, 1e-12);
  LOB_CHECK_EQ(ra.final_q, lob::Qty{1});
  LOB_CHECK_NEAR(ra.profit, 7.0, 1e-12);
  LOB_CHECK_NEAR(ra.profit, ra.cash + static_cast<double>(ra.final_q) * ra.final_mid, 1e-12);
  LOB_CHECK_NEAR(ra.mean_spread, 2.0, 1e-12);

  // Simultaneous fills: step 0 bid at 99; step 1 both sides (bid 100, ask 102), inventory
  // unchanged; step 2 ask at 100. Bought 99 + 100, sold 102 + 100: profit 3, q = 0.
  const Path b = hand_path({100, 101, 99, 102}, {0.0, 0.0, 0.5}, {0.5, 0.0, 0.0});
  const SimResult rb = run_path(b, p, fixed_quotes(1.0, 1.0));
  LOB_CHECK_EQ(rb.final_q, lob::Qty{0});
  LOB_CHECK_NEAR(rb.profit, 3.0, 1e-12);
  LOB_CHECK_EQ(rb.bid_fills, 2);
  LOB_CHECK_EQ(rb.ask_fills, 2);
}

LOB_TEST(inventory_strategy_quotes_around_reservation_price) {
  const ModelParams p = tiny_model();
  const double gamma = 0.5;
  // Bid fills at step 0, so the agent is long one unit at step 1 and shades both quotes down.
  const Path path = hand_path({100, 100, 100, 100}, {0.0, 0.5, 0.5}, {0.5, 0.5, 0.5});
  std::vector<TraceRow> trace;
  simulate(path, Strategy::Inventory, gamma, p, &trace);
  LOB_CHECK_EQ(trace.size(), std::size_t{3});
  const double t1 = 1.0 / 3.0;
  const double r1 = 100.0 - 1.0 * gamma * 4.0 * (1.0 - t1);
  const double half = 0.5 * optimal_spread(t1, gamma, p);
  LOB_CHECK_EQ(trace[1].q, lob::Qty{1});
  LOB_CHECK_NEAR(trace[1].bid, r1 - half, 1e-12);
  LOB_CHECK_NEAR(trace[1].ask, r1 + half, 1e-12);
  LOB_CHECK_NEAR(trace[0].bid + trace[0].ask, 200.0, 1e-12);  // flat: centred on the mid
}

LOB_TEST(symmetric_strategy_has_same_average_spread) {
  const ModelParams p;
  Path path;
  generate_path(99, 0, p, path);
  for (double gamma : {0.01, 0.1, 1.0}) {
    std::vector<TraceRow> ti, ts, tv;
    const SimResult ri = simulate(path, Strategy::Inventory, gamma, p, &ti);
    const SimResult rs = simulate(path, Strategy::Symmetric, gamma, p, &ts);
    const SimResult rv = simulate(path, Strategy::SymmetricTimeMatched, gamma, p, &tv);
    LOB_CHECK_NEAR(rs.mean_spread, ri.mean_spread, 1e-12);
    LOB_CHECK_NEAR(rv.mean_spread, ri.mean_spread, 1e-12);
    LOB_CHECK_NEAR(ri.mean_spread, average_optimal_spread(gamma, p), 1e-12);
    for (std::size_t n = 0; n < ti.size(); ++n) {
      LOB_CHECK_NEAR(ts[n].ask - ts[n].bid, ri.mean_spread, 1e-9);           // constant
      LOB_CHECK_NEAR(tv[n].ask - tv[n].bid, ti[n].ask - ti[n].bid, 1e-9);    // matched per step
      LOB_CHECK_NEAR(0.5 * (ts[n].bid + ts[n].ask), ts[n].mid, 1e-9);        // centred on mid
      LOB_CHECK_NEAR(0.5 * (tv[n].bid + tv[n].ask), tv[n].mid, 1e-9);
    }
  }
}

LOB_TEST(seed_reproducibility_and_paired_paths) {
  ExperimentConfig cfg;
  cfg.gammas = {0.1, 1.0};
  cfg.strategies = {Strategy::Inventory, Strategy::Symmetric};
  cfg.sims = 200;
  cfg.seed = 2024;
  const ExperimentResults a = run_experiment(cfg);
  const ExperimentResults b = run_experiment(cfg);
  cfg.seed = 2025;
  const ExperimentResults c = run_experiment(cfg);
  bool same = true, differs = false, paired = true;
  for (std::size_t g = 0; g < 2; ++g) {
    for (std::size_t s = 0; s < 2; ++s) {
      for (std::size_t i = 0; i < 200; ++i) {
        same = same && a[g][s][i].profit == b[g][s][i].profit &&
               a[g][s][i].final_q == b[g][s][i].final_q;
        differs = differs || a[g][s][i].profit != c[g][s][i].profit;
        // Every gamma and strategy sees the same final mid for simulation i.
        paired = paired && a[g][s][i].final_mid == a[0][0][i].final_mid;
      }
    }
  }
  LOB_CHECK(same);
  LOB_CHECK(differs);
  LOB_CHECK(paired);
}

LOB_TEST(mid_increments_have_the_model_variance) {
  const ModelParams p;  // per-step sd = 2 sqrt(0.005) ~ 0.1414
  Path path;
  double sum = 0.0, sum_sq = 0.0;
  long n = 0;
  for (std::uint64_t i = 0; i < 500; ++i) {
    generate_path(5, i, p, path);
    for (std::size_t j = 0; j + 1 < path.mid.size(); ++j) {
      const double d = path.mid[j + 1] - path.mid[j];
      sum += d;
      sum_sq += d * d;
      ++n;
    }
  }
  const double var = sum_sq / static_cast<double>(n);
  const double expected = p.sigma * p.sigma * p.dt();
  LOB_CHECK_NEAR(sum / static_cast<double>(n), 0.0, 5.0 * std::sqrt(expected / 100000.0));
  // Var of a sample variance of normals: 2 sigma^4 / n.
  LOB_CHECK_NEAR(var, expected, 5.0 * expected * std::sqrt(2.0 / 100000.0));
}

LOB_TEST(quantile_type7) {
  const std::vector<double> v = {1.0, 2.0, 3.0, 4.0};
  LOB_CHECK_NEAR(quantile_sorted(v, 0.0), 1.0, 1e-15);
  LOB_CHECK_NEAR(quantile_sorted(v, 0.5), 2.5, 1e-15);
  LOB_CHECK_NEAR(quantile_sorted(v, 1.0), 4.0, 1e-15);
  LOB_CHECK_NEAR(quantile_sorted(v, 0.025), 1.075, 1e-12);
}

LOB_TEST(bootstrap_of_identical_strategies_is_degenerate) {
  ExperimentConfig cfg;
  cfg.gammas = {0.1};
  cfg.strategies = {Strategy::Symmetric};
  cfg.sims = 300;
  const ExperimentResults r = run_experiment(cfg);
  lob::Rng rng(3);
  const PairedComparison c = paired_bootstrap(r[0][0], r[0][0], 200, rng);
  LOB_CHECK_NEAR(c.sd_profit_ratio.lo, 1.0, 1e-12);
  LOB_CHECK_NEAR(c.sd_profit_ratio.hi, 1.0, 1e-12);
  LOB_CHECK_NEAR(c.mean_profit_rel_diff.estimate, 0.0, 1e-12);
  LOB_CHECK_NEAR(c.mean_profit_rel_diff.hi, 0.0, 1e-12);
}

LOB_TEST_MAIN()
