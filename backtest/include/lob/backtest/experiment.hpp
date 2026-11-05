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

// "none", "naive", "as:<gamma>", "sym:<gamma>", "symtv:<gamma>", "skewc:<gamma>",
// "fixed:<offset ticks>".
struct StrategySpec {
  enum class Kind { None, AS, Sym, Naive, Fixed, SymTV, SkewC };
  Kind kind = Kind::None;
  double param = 0.0;  // gamma for AS, Sym, SymTV and SkewC, offset for Fixed
  std::string label;   // the spec string as given
};

// Parses a comma-separated list. Returns false and sets `err` on a bad entry.
bool parse_strategies(const std::string& list, std::vector<StrategySpec>& out, std::string& err);

// Estimates used by the A&S formulas (see calibrate below): sigma in ticks/sqrt(s), k per tick.
struct AsCalibration {
  double sigma = 0.0;
  double k = 0.0;
};

// True for the strategies built from the A&S inputs (as, sym, symtv, skewc).
bool uses_as_inputs(StrategySpec::Kind k) noexcept;

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
// of ln lambda on delta over the points with at least `min_count` orders, or, when `points` is
// given, over exactly the points i with (*points)[i] true (i indexes delta = 0.5 (i + 1); used to
// refit a subsample on the points of the full-sample fit).
struct KFit {
  double A = 0, k = 0, r2 = 0;
  std::vector<double> delta, rate;
  std::vector<std::int64_t> count;
  std::vector<bool> used;
};
KFit fit_k(const std::vector<EpisodeResult>& eps, double horizon, std::int64_t min_count,
           const std::vector<bool>* points = nullptr);

// sigma (10 s scale) and k for consecutive blocks of episodes: block b holds episodes
// [b n / blocks, (b + 1) n / blocks). Each block's k fit uses the points of `full` (fit_k above),
// so block estimates differ by sampling only. Used for the spread of the estimates.
struct CalibrationBlock {
  int first = 0, episodes = 0;
  double sigma = 0, k = 0, A = 0;
  int points = 0;  // points used in the block's k fit
};
std::vector<CalibrationBlock> calibration_blocks(const std::vector<EpisodeResult>& eps,
                                                 double horizon, int blocks, const KFit& full);

struct Interval {
  double estimate = 0, lo = 0, hi = 0;
};
struct NamedInterval {
  std::string name;
  Interval iv;
  int kept = 0;  // bootstrap replicates with a finite value (the others are dropped)
};
// Statistics of a versus b: mean_pnl_diff, sd_pnl_ratio, sd_final_inv_ratio, mean_abs_inv_diff,
// sharpe_diff, fill_rate_ratio, markout_1s_diff, markout_10s_diff. Paired percentile bootstrap:
// each replicate draws episode indices with replacement and uses the same indices for a and b.
// A replicate whose statistic is not finite (a zero SD in a ratio, no fills) is dropped for that
// statistic; `kept` counts the replicates behind each interval.
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
// Pairs compared, when both members are present: (as:g, sym:g), (as:g, naive) and (sym:g, naive)
// for every g, then (as:g, symtv:g) and (symtv:g, sym:g), then (as:g, skewc:g) and
// (skewc:g, sym:g). Pairs with a newer arm come after the older ones, so that adding symtv or
// skewc arms leaves the index (and so the bootstrap stream) of every older pair unchanged.
// Returned as indices into `specs`.
std::vector<std::pair<std::size_t, std::size_t>> comparison_pairs(
    const std::vector<StrategySpec>& specs);

// The 2 x 2 design of the skew/spread decomposition at one gamma: A&S (skew and time-varying
// spread), SkewC (skew only), SymTV (time-varying spread only), Symmetric (neither).
struct FactorialArms {
  std::size_t as = 0, skewc = 0, symtv = 0, sym = 0;  // indices into the strategy list
};
// One entry per as:g whose skewc:g, symtv:g and sym:g are all in `specs`, in list order.
std::vector<FactorialArms> factorial_sets(const std::vector<StrategySpec>& specs);

// Interaction of the skew and the time-varying spread, with a paired percentile bootstrap that
// draws one set of episode indices per replicate and applies it to all four arms:
//   sd_pnl_interaction       = (SD_AS / SD_SkewC) / (SD_SymTV / SD_Sym) of PnL,
//   sd_final_inv_interaction = the same for terminal inventory q_T,
//   mean_pnl_interaction     = (mean_AS - mean_SkewC) - (mean_SymTV - mean_Sym), USD.
// The ratios are 1 (the difference 0) when the spread term has the same effect with and without
// the skew. A replicate with a zero SD in a ratio is dropped and counted in `kept`.
std::vector<NamedInterval> interaction_bootstrap(const std::vector<EpisodeResult>& as,
                                                 const std::vector<EpisodeResult>& skewc,
                                                 const std::vector<EpisodeResult>& symtv,
                                                 const std::vector<EpisodeResult>& sym,
                                                 int replicates, Rng& rng, double level = 0.95);

}  // namespace lob::backtest
