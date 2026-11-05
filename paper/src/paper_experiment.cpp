#include "lob/paper_experiment.hpp"

#include <algorithm>
#include <cmath>

namespace lob::paper {

ExperimentResults run_experiment(const ExperimentConfig& cfg) {
  const auto n = static_cast<std::size_t>(cfg.sims);
  ExperimentResults out(cfg.gammas.size(),
                        std::vector<std::vector<SimResult>>(cfg.strategies.size(),
                                                            std::vector<SimResult>(n)));
  // Rules are built once per cell: the symmetric average spread costs a loop over the grid.
  std::vector<std::vector<QuotingRule>> rules(cfg.gammas.size());
  for (std::size_t g = 0; g < cfg.gammas.size(); ++g) {
    for (Strategy s : cfg.strategies) rules[g].emplace_back(s, cfg.gammas[g], cfg.model);
  }
  Path path;
  for (std::size_t i = 0; i < n; ++i) {
    generate_path(cfg.seed, i, cfg.model, path);
    for (std::size_t g = 0; g < cfg.gammas.size(); ++g) {
      for (std::size_t s = 0; s < cfg.strategies.size(); ++s) {
        out[g][s][i] = run_path(path, cfg.model, rules[g][s]);
      }
    }
  }
  return out;
}

Summary summarize(const std::vector<SimResult>& sims, std::size_t first, std::size_t count) {
  Summary r;
  r.n = static_cast<int>(count);
  if (count == 0) return r;
  double spread = 0.0, profit = 0.0, q = 0.0;
  for (std::size_t i = first; i < first + count; ++i) {
    spread += sims[i].mean_spread;
    profit += sims[i].profit;
    q += static_cast<double>(sims[i].final_q);
  }
  const auto c = static_cast<double>(count);
  r.mean_spread = spread / c;
  r.mean_profit = profit / c;
  r.mean_q = q / c;
  if (count < 2) return r;
  double ss_profit = 0.0, ss_q = 0.0;  // two-pass for accuracy
  for (std::size_t i = first; i < first + count; ++i) {
    const double dp = sims[i].profit - r.mean_profit;
    const double dq = static_cast<double>(sims[i].final_q) - r.mean_q;
    ss_profit += dp * dp;
    ss_q += dq * dq;
  }
  r.sd_profit = std::sqrt(ss_profit / (c - 1.0));
  r.sd_q = std::sqrt(ss_q / (c - 1.0));
  return r;
}

Summary summarize(const std::vector<SimResult>& sims) { return summarize(sims, 0, sims.size()); }

double quantile_sorted(const std::vector<double>& sorted, double prob) {
  const double h = (static_cast<double>(sorted.size()) - 1.0) * prob;
  const auto lo = static_cast<std::size_t>(std::floor(h));
  const std::size_t hi = std::min(lo + 1, sorted.size() - 1);
  return sorted[lo] + (h - static_cast<double>(lo)) * (sorted[hi] - sorted[lo]);
}

namespace {

// Running sums for a sample variance. Values are shifted by a fixed centre (the full-sample
// mean) so that sum-of-squares does not lose precision.
struct Moments {
  double centre = 0.0;
  double sum = 0.0;
  double sum_sq = 0.0;
  void add(double x) {
    const double d = x - centre;
    sum += d;
    sum_sq += d * d;
  }
  double mean(double n) const { return centre + sum / n; }
  double sd(double n) const {
    return std::sqrt(std::max(0.0, (sum_sq - sum * sum / n) / (n - 1.0)));
  }
};

PairedComparison point_estimates(const Summary& a, const Summary& b) {
  PairedComparison c;
  c.sd_profit_ratio.estimate = a.sd_profit / b.sd_profit;
  c.sd_q_ratio.estimate = a.sd_q / b.sd_q;
  c.mean_profit_rel_diff.estimate = (a.mean_profit - b.mean_profit) / b.mean_profit;
  return c;
}

}  // namespace

PairedComparison paired_bootstrap(const std::vector<SimResult>& a, const std::vector<SimResult>& b,
                                  int replicates, Rng& rng, double level) {
  const Summary sa = summarize(a);
  const Summary sb = summarize(b);
  PairedComparison out = point_estimates(sa, sb);

  const std::size_t n = a.size();
  const auto nd = static_cast<double>(n);
  // The four paired values of simulation i side by side: one cache line per random draw.
  struct Pair {
    double profit_a, q_a, profit_b, q_b;
  };
  std::vector<Pair> pairs(n);
  for (std::size_t i = 0; i < n; ++i) {
    pairs[i] = {a[i].profit, static_cast<double>(a[i].final_q), b[i].profit,
                static_cast<double>(b[i].final_q)};
  }
  const auto reps = static_cast<std::size_t>(replicates);
  std::vector<double> r_profit(reps), r_q(reps), r_mean(reps);
  for (std::size_t rep = 0; rep < reps; ++rep) {
    Moments pa{sa.mean_profit}, qa{sa.mean_q}, pb{sb.mean_profit}, qb{sb.mean_q};
    for (std::size_t j = 0; j < n; ++j) {
      const Pair& x = pairs[rng.uniform_int(static_cast<std::uint64_t>(n))];
      pa.add(x.profit_a);
      qa.add(x.q_a);
      pb.add(x.profit_b);
      qb.add(x.q_b);
    }
    r_profit[rep] = pa.sd(nd) / pb.sd(nd);
    r_q[rep] = qa.sd(nd) / qb.sd(nd);
    r_mean[rep] = (pa.mean(nd) - pb.mean(nd)) / pb.mean(nd);
  }
  const double p_lo = 0.5 * (1.0 - level);
  const double p_hi = 0.5 * (1.0 + level);
  auto fill = [&](std::vector<double>& v, Interval& iv) {
    std::sort(v.begin(), v.end());
    iv.lo = quantile_sorted(v, p_lo);
    iv.hi = quantile_sorted(v, p_hi);
  };
  fill(r_profit, out.sd_profit_ratio);
  fill(r_q, out.sd_q_ratio);
  fill(r_mean, out.mean_profit_rel_diff);
  return out;
}

}  // namespace lob::paper
