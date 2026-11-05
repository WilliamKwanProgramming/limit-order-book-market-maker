// Experiment helpers: strategy parsing, comparison pairs, pooled summaries, the k fit and the
// paired bootstrap.
#include <algorithm>
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
  LOB_CHECK(parse_strategies("skewc:5e-3", s, err));
  LOB_CHECK(s[0].kind == StrategySpec::Kind::SkewC && s[0].param == 0.005);
  LOB_CHECK_EQ(s[0].label, std::string("skewc:5e-3"));
  LOB_CHECK(uses_as_inputs(StrategySpec::Kind::SymTV) && uses_as_inputs(StrategySpec::Kind::Sym) &&
            uses_as_inputs(StrategySpec::Kind::AS) && uses_as_inputs(StrategySpec::Kind::SkewC) &&
            !uses_as_inputs(StrategySpec::Kind::Naive));
  for (const char* bad : {"as", "as:0", "as:-1", "sym:x", "fixed:-1", "foo", "naive:1", "",
                          "symtv", "symtv:0", "skewc", "skewc:0", "skewc:-1", "skewc:x"}) {
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

LOB_TEST(skewc_pairs_come_after_the_symtv_pairs) {
  std::vector<StrategySpec> s, old;
  std::string err;
  // The published list order: as/sym per gamma, then symtv per gamma; skewc appended last.
  LOB_CHECK(parse_strategies("none,naive,as:0.1,sym:0.1,as:0.2,sym:0.2,symtv:0.1,symtv:0.2", old,
                             err));
  LOB_CHECK(parse_strategies(
      "none,naive,as:0.1,sym:0.1,as:0.2,sym:0.2,symtv:0.1,symtv:0.2,skewc:0.1,skewc:0.2", s, err));
  const auto po = comparison_pairs(old);
  const auto p = comparison_pairs(s);
  LOB_CHECK_EQ(p.size(), po.size() + 4);
  for (std::size_t i = 0; i < po.size(); ++i) LOB_CHECK(p[i] == po[i]);
  const std::size_t n = po.size();
  // as:g vs skewc:g for each g, then skewc:g vs sym:g.
  LOB_CHECK(p[n] == std::make_pair(std::size_t{2}, std::size_t{8}));
  LOB_CHECK(p[n + 1] == std::make_pair(std::size_t{4}, std::size_t{9}));
  LOB_CHECK(p[n + 2] == std::make_pair(std::size_t{8}, std::size_t{3}));
  LOB_CHECK(p[n + 3] == std::make_pair(std::size_t{9}, std::size_t{5}));
  // A skewc arm without its partner arm forms no pair.
  LOB_CHECK(parse_strategies("as:0.1,sym:0.2,skewc:0.3", s, err));
  LOB_CHECK(comparison_pairs(s).empty());
}

LOB_TEST(factorial_sets_need_all_four_arms_at_one_gamma) {
  std::vector<StrategySpec> s;
  std::string err;
  LOB_CHECK(parse_strategies(
      "naive,as:0.1,sym:0.1,as:0.2,sym:0.2,symtv:0.1,symtv:0.2,skewc:0.1,skewc:0.3", s, err));
  const auto f = factorial_sets(s);
  LOB_CHECK_EQ(f.size(), std::size_t{1});  // gamma 0.2 has no skewc arm
  LOB_CHECK(f[0].as == 1 && f[0].skewc == 7 && f[0].symtv == 5 && f[0].sym == 2);
  LOB_CHECK(parse_strategies("as:0.1,sym:0.1,symtv:0.1", s, err));
  LOB_CHECK(factorial_sets(s).empty());
}

namespace {

std::vector<EpisodeResult> episodes(std::size_t n, Rng& rng, double sd) {
  std::vector<EpisodeResult> e(n);
  for (EpisodeResult& r : e) {
    r.pnl = rng.normal(0.0, sd);
    r.final_inventory = static_cast<Qty>(rng.uniform_int(std::int64_t{-6}, std::int64_t{6}));
  }
  return e;
}

// The interval of the named statistic; a missing name gives kept = -1 and NaN values, which fail
// every check below.
NamedInterval stat(const std::vector<NamedInterval>& v, const std::string& name) {
  for (const NamedInterval& ni : v) {
    if (ni.name == name) return ni;
  }
  NamedInterval missing;
  missing.kept = -1;
  missing.iv = {std::nan(""), std::nan(""), std::nan("")};
  return missing;
}

}  // namespace

LOB_TEST(interaction_uses_the_same_episodes_for_all_four_arms) {
  // A&S = 3 x SkewC and SymTV = Symmetric episode by episode: the PnL SD interaction is exactly 3
  // and the q_T SD interaction exactly 2 in every replicate, but only if the four arms are
  // resampled with the same indices. The mean interaction is 2 x the SkewC mean, which varies.
  Rng rng(31);
  const auto skewc = episodes(300, rng, 10.0);
  const auto sym = episodes(300, rng, 25.0);
  auto as = skewc;
  for (std::size_t i = 0; i < as.size(); ++i) {
    as[i].pnl = 3.0 * skewc[i].pnl;
    as[i].final_inventory = 2 * skewc[i].final_inventory;
  }
  Rng b(32);
  const auto out = interaction_bootstrap(as, skewc, sym, sym, 500, b);
  LOB_CHECK_EQ(out.size(), std::size_t{3});
  const NamedInterval sp = stat(out, "sd_pnl_interaction");
  LOB_CHECK_NEAR(sp.iv.estimate, 3.0, 1e-12);
  LOB_CHECK_NEAR(sp.iv.lo, 3.0, 1e-12);
  LOB_CHECK_NEAR(sp.iv.hi, 3.0, 1e-12);
  LOB_CHECK_EQ(sp.kept, 500);
  const NamedInterval si = stat(out, "sd_final_inv_interaction");
  LOB_CHECK_NEAR(si.iv.lo, 2.0, 1e-12);
  LOB_CHECK_NEAR(si.iv.hi, 2.0, 1e-12);
  const NamedInterval m = stat(out, "mean_pnl_interaction");
  double mean = 0;
  for (const EpisodeResult& r : skewc) mean += r.pnl;
  mean /= static_cast<double>(skewc.size());
  LOB_CHECK_NEAR(m.iv.estimate, 2.0 * mean, 1e-9);
  LOB_CHECK(m.iv.lo < m.iv.estimate && m.iv.estimate < m.iv.hi);
}

LOB_TEST(interaction_is_the_ratio_of_the_paired_ratios) {
  // On unrelated arms the point estimates equal (A&S/SkewC) / (SymTV/Sym) from paired_bootstrap,
  // and a known shift in the means is recovered with a narrow interval.
  Rng rng(41);
  const auto as = episodes(400, rng, 12.0), skewc = episodes(400, rng, 15.0),
             tv = episodes(400, rng, 30.0);
  auto sym = tv;
  for (std::size_t i = 0; i < sym.size(); ++i) sym[i].pnl = tv[i].pnl - 4.0 + rng.normal(0.0, 0.1);
  auto as2 = skewc;
  for (std::size_t i = 0; i < as2.size(); ++i) as2[i].pnl = skewc[i].pnl + 1.0;
  Rng b(42), c(43), d(44);
  const auto out = interaction_bootstrap(as, skewc, tv, sym, 200, b);
  const auto r1 = paired_bootstrap(as, skewc, 600.0, 10, c);
  const auto r2 = paired_bootstrap(tv, sym, 600.0, 10, d);
  LOB_CHECK_NEAR(stat(out, "sd_pnl_interaction").iv.estimate,
                 stat(r1, "sd_pnl_ratio").iv.estimate / stat(r2, "sd_pnl_ratio").iv.estimate,
                 1e-12);
  LOB_CHECK_NEAR(stat(out, "sd_final_inv_interaction").iv.estimate,
                 stat(r1, "sd_final_inv_ratio").iv.estimate /
                     stat(r2, "sd_final_inv_ratio").iv.estimate,
                 1e-12);
  LOB_CHECK_NEAR(stat(out, "mean_pnl_interaction").iv.estimate,
                 stat(r1, "mean_pnl_diff").iv.estimate - stat(r2, "mean_pnl_diff").iv.estimate,
                 1e-9);
  // (A&S2 - SkewC) - (SymTV - Sym) = 1 - 4 = -3 per episode, up to the 0.1 noise.
  Rng e(45);
  const NamedInterval m = stat(interaction_bootstrap(as2, skewc, tv, sym, 500, e),
                                "mean_pnl_interaction");
  LOB_CHECK(m.iv.lo < -3.0 && m.iv.hi > -3.0 && m.iv.hi - m.iv.lo < 0.05);
  // A constant arm has SD 0: the ratio statistics are dropped, the mean difference is kept.
  auto flat = sym;
  for (EpisodeResult& r : flat) r.pnl = 1.0;
  Rng f(46);
  const auto z = interaction_bootstrap(as, skewc, tv, flat, 50, f);
  LOB_CHECK_EQ(stat(z, "sd_pnl_interaction").kept, 0);
  LOB_CHECK(std::isnan(stat(z, "sd_pnl_interaction").iv.lo));
  LOB_CHECK_EQ(stat(z, "mean_pnl_interaction").kept, 50);
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

LOB_TEST(k_fit_on_given_points_and_calibration_blocks) {
  const double k = 1.3, A = 0.9, horizon = 5e8;
  EpisodeResult r;
  std::vector<double> ge(kReachBins + 1, 0.0);
  for (std::size_t j = 1; j < 20; ++j) ge[j] = std::round(1e9 * A * std::exp(-k * 0.5 * static_cast<double>(j)));
  for (std::size_t j = 1; j < 20; ++j)
    r.reach_hist[j] = static_cast<std::int64_t>(ge[j] - ge[j + 1]);
  const KFit by_count = fit_k({r}, horizon, static_cast<std::int64_t>(ge[10]) + 1);
  // Fitting on the same points given explicitly gives the same fit, whatever the counts.
  const KFit by_mask = fit_k({r}, horizon, 0, &by_count.used);
  LOB_CHECK(by_mask.used == by_count.used);
  LOB_CHECK_EQ(by_mask.k, by_count.k);
  LOB_CHECK_EQ(by_mask.A, by_count.A);
  std::vector<bool> two(by_count.used.size(), false);
  two[2] = two[5] = true;
  const KFit h = fit_k({r}, horizon, 0, &two);
  LOB_CHECK_EQ(std::count(h.used.begin(), h.used.end(), true), 2);
  LOB_CHECK_NEAR(h.k, k, 1e-5);
  // Blocks of identical episodes: every block gives the full-sample estimates. Ten episodes in
  // three blocks: 3, 3 and 4 episodes, consecutive.
  r.vol_ss[2] = 40.0;
  r.vol_n[2] = 6;
  std::vector<EpisodeResult> eps(10, r);
  const KFit full = fit_k(eps, horizon, 100);
  const auto blocks = calibration_blocks(eps, horizon, 3, full);
  LOB_CHECK_EQ(blocks.size(), std::size_t{3});
  LOB_CHECK_EQ(blocks[0].first, 0);
  LOB_CHECK_EQ(blocks[1].first, 3);
  LOB_CHECK_EQ(blocks[2].first, 6);
  LOB_CHECK_EQ(blocks[2].episodes, 4);
  for (const CalibrationBlock& b : blocks) {
    LOB_CHECK_NEAR(b.k, full.k, 1e-9);
    LOB_CHECK_NEAR(b.sigma, std::sqrt(40.0 / (6 * 10.0)), 1e-12);
    LOB_CHECK_EQ(b.points, static_cast<int>(std::count(full.used.begin(), full.used.end(), true)));
  }
  // A block with different data gives a different estimate.
  eps[0].vol_ss[2] = 400.0;
  const auto b2 = calibration_blocks(eps, horizon, 3, full);
  LOB_CHECK(b2[0].sigma > blocks[0].sigma && b2[1].sigma == blocks[1].sigma);
}

LOB_TEST(bootstrap_counts_dropped_replicates) {
  std::vector<EpisodeResult> a(50), b(50);
  Rng rng(3);
  for (std::size_t i = 0; i < a.size(); ++i) {
    a[i].pnl = rng.normal(0.0, 1.0);
    b[i].pnl = 7.0;  // constant: SD 0 in every replicate, so the SD ratio is never finite
    a[i].final_inventory = static_cast<Qty>(i % 3);
    b[i].final_inventory = static_cast<Qty>(i % 5);
  }
  Rng r(4);
  const auto out = paired_bootstrap(a, b, 600.0, 100, r);
  for (const NamedInterval& ni : out) {
    if (ni.name == "sd_pnl_ratio") {
      LOB_CHECK_EQ(ni.kept, 0);
      LOB_CHECK(std::isnan(ni.iv.lo) && std::isnan(ni.iv.hi));
    }
    if (ni.name == "mean_pnl_diff") LOB_CHECK_EQ(ni.kept, 100);
  }
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
