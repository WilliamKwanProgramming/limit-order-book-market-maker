// Experiment helpers: strategy parsing, comparison pairs, pooled summaries, the k fit and the
// paired bootstrap.
#include <cmath>

#include "lob/backtest/experiment.hpp"
#include "lob/check.hpp"

using namespace lob;
using namespace lob::backtest;

LOB_TEST(parses_strategy_lists) {
  std::vector<StrategySpec> s;
  std::string err;
  LOB_CHECK(parse_strategies("none,naive,as:0.01,sym:2e-3,fixed:1.5", s, err));
  LOB_CHECK_EQ(s.size(), std::size_t{5});
  LOB_CHECK(s[2].kind == StrategySpec::Kind::AS && s[2].param == 0.01);
  LOB_CHECK(s[3].kind == StrategySpec::Kind::Sym && s[3].param == 0.002);
  LOB_CHECK(s[4].kind == StrategySpec::Kind::Fixed && s[4].param == 1.5);
  LOB_CHECK_EQ(s[3].label, std::string("sym:2e-3"));
  LOB_CHECK(parse_strategies("symtv:0.004", s, err));
  LOB_CHECK(s[0].kind == StrategySpec::Kind::SymTV && s[0].param == 0.004);
  LOB_CHECK(uses_as_inputs(StrategySpec::Kind::SymTV) && uses_as_inputs(StrategySpec::Kind::Sym) &&
            uses_as_inputs(StrategySpec::Kind::AS) && !uses_as_inputs(StrategySpec::Kind::Naive));
  for (const char* bad : {"as", "as:0", "as:-1", "sym:x", "fixed:-1", "foo", "naive:1", "",
                          "symtv", "symtv:0"}) {
    LOB_CHECK(!parse_strategies(bad, s, err));
  }
}

LOB_TEST(comparison_pairs_match_gammas) {
  std::vector<StrategySpec> s;
  std::string err;
  LOB_CHECK(parse_strategies("naive,as:0.1,sym:0.1,as:0.2,sym:0.3", s, err));
  const auto p = comparison_pairs(s);
  // as:0.1 vs sym:0.1, as:0.1 vs naive, as:0.2 vs naive, sym:0.1 vs naive, sym:0.3 vs naive.
  LOB_CHECK_EQ(p.size(), std::size_t{5});
  LOB_CHECK(p[0] == std::make_pair(std::size_t{1}, std::size_t{2}));
  LOB_CHECK(p[1] == std::make_pair(std::size_t{1}, std::size_t{0}));
  LOB_CHECK(p[2] == std::make_pair(std::size_t{3}, std::size_t{0}));
}

LOB_TEST(symtv_pairs_come_after_the_existing_pairs) {
  std::vector<StrategySpec> s, old;
  std::string err;
  LOB_CHECK(parse_strategies("naive,as:0.1,sym:0.1,as:0.2,sym:0.2", old, err));
  LOB_CHECK(parse_strategies("naive,as:0.1,sym:0.1,as:0.2,sym:0.2,symtv:0.1,symtv:0.2", s, err));
  const auto po = comparison_pairs(old);
  const auto p = comparison_pairs(s);
  // The existing pairs keep their positions (and so their bootstrap streams).
  LOB_CHECK_EQ(p.size(), po.size() + 4);
  for (std::size_t i = 0; i < po.size(); ++i) LOB_CHECK(p[i] == po[i]);
  const std::size_t n = po.size();
  // as:g vs symtv:g for each g, then symtv:g vs sym:g.
  LOB_CHECK(p[n] == std::make_pair(std::size_t{1}, std::size_t{5}));
  LOB_CHECK(p[n + 1] == std::make_pair(std::size_t{3}, std::size_t{6}));
  LOB_CHECK(p[n + 2] == std::make_pair(std::size_t{5}, std::size_t{2}));
  LOB_CHECK(p[n + 3] == std::make_pair(std::size_t{6}, std::size_t{4}));
}

LOB_TEST(summary_pools_ratios) {
  std::vector<EpisodeResult> e(2);
  e[0].pnl = 10;
  e[1].pnl = 20;
  e[0].final_inventory = 2;
  e[1].final_inventory = -2;
  e[0].abs_inventory_time = 100;
  e[1].abs_inventory_time = 300;
  e[0].fills = 1;
  e[1].fills = 3;
  e[0].fill_lots = 1;
  e[1].fill_lots = 3;
  e[0].quote_seconds = 10;
  e[1].quote_seconds = 30;
  e[0].spread_captured = 0.5;
  e[1].spread_captured = 1.5;
  e[0].markout_sum = {1, 2, 3};
  e[1].markout_sum = {3, 2, 1};
  e[0].fill_dispersion = 1.0;
  e[1].fill_dispersion = std::nan("");
  const StrategySummary s = summarize(e, 100.0);
  LOB_CHECK_NEAR(s.mean_pnl, 15.0, 1e-12);
  LOB_CHECK_NEAR(s.sd_pnl, std::sqrt(50.0), 1e-12);
  LOB_CHECK_NEAR(s.sharpe, 15.0 / std::sqrt(50.0), 1e-12);
  LOB_CHECK_NEAR(s.sd_final_inv, std::sqrt(8.0), 1e-12);
  LOB_CHECK_NEAR(s.mean_abs_inv, 2.0, 1e-12);  // 400 lot-s / (2 x 100 s)
  LOB_CHECK_NEAR(s.fill_rate, 0.1, 1e-12);     // 4 fills / 40 quote-s
  LOB_CHECK_NEAR(s.spread_captured, 0.5, 1e-12);
  LOB_CHECK_NEAR(s.markout[0], 1.0, 1e-12);
  LOB_CHECK_NEAR(s.markout[2], 1.0, 1e-12);
  LOB_CHECK_NEAR(s.quote_share, 0.1, 1e-12);   // 40 / (2 x 100 x 2)
  LOB_CHECK_NEAR(s.fill_dispersion, 1.0, 1e-12);
  LOB_CHECK_EQ(s.dispersion_n, 1);
}

LOB_TEST(k_fit_recovers_an_exact_exponential) {
  // Reach histogram whose survival function is exactly C exp(-k delta) on the half-tick grid.
  // fit_k divides counts by 2 x episodes x horizon; horizon 5e8 s makes that 1e9, so the rates
  // are counts / 1e9 = A exp(-k delta) up to rounding of the counts.
  const double k = 1.3, A = 0.9, horizon = 5e8;
  EpisodeResult r;
  std::vector<double> ge(kReachBins + 1, 0.0);
  for (std::size_t j = 1; j < 20; ++j) ge[j] = std::round(1e9 * A * std::exp(-k * 0.5 * static_cast<double>(j)));
  for (std::size_t j = 1; j < 20; ++j)
    r.reach_hist[j] = static_cast<std::int64_t>(ge[j] - ge[j + 1]);
  const KFit g = fit_k({r}, horizon, 100);
  LOB_CHECK_NEAR(g.k, k, 1e-6);
  LOB_CHECK_NEAR(g.A, A, 1e-5);
  LOB_CHECK_NEAR(g.r2, 1.0, 1e-9);
  // Points under the minimum count are reported but not used.
  const KFit h = fit_k({r}, horizon, static_cast<std::int64_t>(ge[10]) + 1);
  std::size_t used = 0;
  for (bool u : h.used) used += u ? 1 : 0;
  LOB_CHECK_EQ(used, std::size_t{9});
  LOB_CHECK_NEAR(h.k, k, 1e-6);
}

LOB_TEST(bootstrap_of_identical_strategies_is_degenerate) {
  std::vector<EpisodeResult> a(200);
  Rng rng(1);
  for (EpisodeResult& r : a) {
    r.pnl = rng.normal(5.0, 2.0);
    r.final_inventory = static_cast<Qty>(rng.uniform_int(std::int64_t{-5}, std::int64_t{5}));
    r.fills = 10;
    r.fill_lots = 10;
    r.quote_seconds = 100;
    r.markout_sum = {1, 2, 3};
  }
  Rng b(2);
  const auto out = paired_bootstrap(a, a, 600.0, 200, b);
  LOB_CHECK_EQ(out.size(), std::size_t{8});
  for (const NamedInterval& ni : out) {
    const bool ratio = ni.name.find("ratio") != std::string::npos;
    LOB_CHECK_NEAR(ni.iv.estimate, ratio ? 1.0 : 0.0, 1e-12);
    LOB_CHECK_NEAR(ni.iv.lo, ratio ? 1.0 : 0.0, 1e-12);
    LOB_CHECK_NEAR(ni.iv.hi, ratio ? 1.0 : 0.0, 1e-12);
  }
}

LOB_TEST(bootstrap_interval_covers_a_known_shift) {
  // b = a + 3 + small noise: the mean PnL difference a - b is -3 and its interval is narrow and
  // contains -3; pairing makes the interval much narrower than the PnL SD would suggest.
  std::vector<EpisodeResult> a(500), b(500);
  Rng rng(3);
  for (std::size_t i = 0; i < a.size(); ++i) {
    a[i].pnl = rng.normal(0.0, 50.0);
    b[i].pnl = a[i].pnl + 3.0 + rng.normal(0.0, 0.5);
    a[i].final_inventory = b[i].final_inventory = 1 + static_cast<Qty>(i % 3);
  }
  Rng r(4);
  const auto out = paired_bootstrap(a, b, 600.0, 1000, r);
  const Interval& d = out[0].iv;
  LOB_CHECK_EQ(out[0].name, std::string("mean_pnl_diff"));
  LOB_CHECK(d.lo < -3.0 + 0.1 && d.hi > -3.0 - 0.1);
  LOB_CHECK(d.hi - d.lo < 0.3);
  LOB_CHECK(d.lo < d.estimate && d.estimate < d.hi);
}

LOB_TEST_MAIN()
