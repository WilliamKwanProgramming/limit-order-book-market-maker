// Experiments on the backtest simulator: many episodes per strategy on shared seeds, summaries,
// paired bootstrap comparisons, market diagnostics and the A&S parameter estimates.
#pragma once

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "lob/backtest/market.hpp"
#include "lob/backtest/params.hpp"
#include "lob/backtest/strategy.hpp"
#include "lob/rng.hpp"

namespace lob::backtest {

// "none", "naive", "as:<gamma>", "sym:<gamma>", "fixed:<offset ticks>".
struct StrategySpec {
  enum class Kind { None, AS, Sym, Naive, Fixed };
  Kind kind = Kind::None;
  double param = 0.0;  // gamma for AS and Sym, offset for Fixed
  std::string label;   // the spec string as given
};

// Parses a comma-separated list. Returns false and sets `err` on a bad entry.
bool parse_strategies(const std::string& list, std::vector<StrategySpec>& out, std::string& err);

// Estimates used by the A&S formulas (see calibrate below): sigma in ticks/sqrt(s), k per tick.
struct AsCalibration {
  double sigma = 0.0;
  double k = 0.0;
};

std::unique_ptr<Strategy> make_strategy(const StrategySpec& s, const SimParams& p,
                                        const AsCalibration& c);

// results[s][e]: strategy s, episode e. Episode e uses episode_seed(seed, e): one burn-in, then a
// copy of the burned-in market per strategy, so every strategy sees the same exogenous events.
using CellResults = std::vector<std::vector<EpisodeResult>>;
CellResults run_cell(const SimParams& p, const std::vector<StrategySpec>& specs,
                     const AsCalibration& c, std::uint64_t seed, int episodes);

struct StrategySummary {
  int n = 0;
  double mean_pnl = 0, sd_pnl = 0, sharpe = 0;
  double mean_final_inv = 0, sd_final_inv = 0;
  double mean_abs_inv = 0;  // time average of |q| over the episode, lots
  double max_abs_inv = 0;
  double mean_fills = 0;
  double fill_rate = 0;        // fills per quote-second (pooled)
  double spread_captured = 0;  // ticks per lot (pooled)
  double markout[kMarkoutHorizons.size()] = {};  // ticks per lot (pooled)
  double quote_dist = 0;       // time-average distance of our resting quotes from the mid, ticks
  double quote_share = 0;      // quote-seconds / (2 T): fraction of time a side is quoted
  double fill_dispersion = 0;  // mean over episodes with fills of var/mean of fills per 1 s
  int dispersion_n = 0;
  double post_only_rejects = 0, limit_rejects = 0;  // per episode
  std::int64_t invariant_failures = 0;
};
StrategySummary summarize(const std::vector<EpisodeResult>& eps, double horizon);

struct MarketDiagnostics {
  double sigma[kVolScales.size()] = {};  // realized volatility, ticks/sqrt(s), per sampling scale
  double spread = 0;          // time-average others' spread, ticks
  double touch_depth = 0;     // time-average others' depth at the best price, lots per side
  double two_sided = 0;       // fraction of time both sides are non-empty
  double mo_rate = 0;         // market orders per second (background + informed)
  double informed_rate = 0;   // informed market orders per second
  double mo_dispersion = 0;   // mean var/mean of market orders per 1 s bin
};
MarketDiagnostics diagnose(const std::vector<EpisodeResult>& eps, double horizon);

// k estimate: rate per side lambda(delta) of market orders whose deepest execution lies at least
// delta ticks from the mid before the order, for delta = 0.5, 1, 1.5, ...; ordinary least squares
// of ln lambda on delta over the points with at least `min_count` orders.
struct KFit {
  double A = 0, k = 0, r2 = 0;
  std::vector<double> delta, rate;
  std::vector<std::int64_t> count;
  std::vector<bool> used;
};
KFit fit_k(const std::vector<EpisodeResult>& eps, double horizon, std::int64_t min_count);

struct Interval {
  double estimate = 0, lo = 0, hi = 0;
};
struct NamedInterval {
  std::string name;
  Interval iv;
};
// Statistics of a versus b: mean_pnl_diff, sd_pnl_ratio, sd_final_inv_ratio, mean_abs_inv_diff,
// sharpe_diff, fill_rate_ratio, markout_1s_diff, markout_10s_diff. Paired percentile bootstrap:
// each replicate draws episode indices with replacement and uses the same indices for a and b.
std::vector<NamedInterval> paired_bootstrap(const std::vector<EpisodeResult>& a,
                                            const std::vector<EpisodeResult>& b, double horizon,
                                            int replicates, Rng& rng, double level = 0.95);

// CSV helpers shared by backtest_run and stress_test. `prefix` columns come first.
void write_summary_header(std::FILE* f, const char* prefix);
void write_summary_row(std::FILE* f, const std::string& prefix, const StrategySpec& spec,
                       const StrategySummary& s);
void write_diagnostics_header(std::FILE* f, const char* prefix);
void write_diagnostics_row(std::FILE* f, const std::string& prefix, const StrategySpec& spec,
                           const MarketDiagnostics& d);
// Pairs compared: (as:g, sym:g), (as:g, naive) and (sym:g, naive) for every g, when both
// members are present. Returned as indices into `specs`.
std::vector<std::pair<std::size_t, std::size_t>> comparison_pairs(
    const std::vector<StrategySpec>& specs);

}  // namespace lob::backtest
