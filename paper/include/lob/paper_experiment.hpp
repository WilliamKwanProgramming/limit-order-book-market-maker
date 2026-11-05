// Monte Carlo experiment around the Avellaneda-Stoikov simulation: many simulations for several
// gammas and strategies on shared random paths, summary statistics, and a paired bootstrap.
#pragma once

#include <cstdint>
#include <vector>

#include "lob/avellaneda_stoikov.hpp"
#include "lob/rng.hpp"

namespace lob::paper {

struct ExperimentConfig {
  ModelParams model;
  std::vector<double> gammas;
  std::vector<Strategy> strategies;
  std::uint64_t seed = 1;
  int sims = 1000;
};

// results[g][s][i]: gamma index g, strategy index s, simulation i. Simulation i uses
// generate_path(seed, i), so every (gamma, strategy) cell sees the same paths.
using ExperimentResults = std::vector<std::vector<std::vector<SimResult>>>;

ExperimentResults run_experiment(const ExperimentConfig& cfg);

struct Summary {
  int n = 0;
  double mean_spread = 0.0;
  double mean_profit = 0.0;
  double sd_profit = 0.0;  // sample standard deviation (divisor n - 1)
  double mean_q = 0.0;
  double sd_q = 0.0;
};

// Summary of sims[first, first + count).
Summary summarize(const std::vector<SimResult>& sims, std::size_t first, std::size_t count);
Summary summarize(const std::vector<SimResult>& sims);

// Quantile of sorted data with linear interpolation between order statistics
// (Hyndman-Fan type 7, the default of R and numpy). Requires a non-empty sorted vector.
double quantile_sorted(const std::vector<double>& sorted, double prob);

struct Interval {
  double estimate = 0.0;  // statistic on the original sample
  double lo = 0.0;
  double hi = 0.0;
};

// Comparison of strategy a (inventory) against strategy b (a symmetric benchmark).
struct PairedComparison {
  Interval sd_profit_ratio;       // sd(profit_a) / sd(profit_b)
  Interval sd_q_ratio;            // sd(final_q_a) / sd(final_q_b)
  Interval mean_profit_rel_diff;  // (mean(profit_a) - mean(profit_b)) / mean(profit_b)
};

// Paired percentile bootstrap. Each replicate draws n simulation indices uniformly with
// replacement, applies the same indices to a and b (they share paths), and recomputes the three
// statistics. The interval is the (1 - level) / 2 and (1 + level) / 2 type-7 quantiles of the
// replicates. Requires a.size() == b.size() >= 2 and replicates >= 1.
PairedComparison paired_bootstrap(const std::vector<SimResult>& a, const std::vector<SimResult>& b,
                                  int replicates, Rng& rng, double level = 0.95);

}  // namespace lob::paper
