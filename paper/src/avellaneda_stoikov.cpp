#include "lob/avellaneda_stoikov.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace lob::paper {

double reservation_price(double s, double q, double t, double gamma, const ModelParams& p) {
  return s - q * gamma * p.sigma * p.sigma * (p.horizon - t);
}

double optimal_spread(double t, double gamma, const ModelParams& p) {
  // log1p keeps full precision as gamma / k -> 0, where the spread tends to 2 / k.
  return gamma * p.sigma * p.sigma * (p.horizon - t) + (2.0 / gamma) * std::log1p(gamma / p.k);
}

double fill_probability(double delta, const ModelParams& p) {
  return std::min(1.0, p.A * std::exp(-p.k * delta) * p.dt());
}

double average_optimal_spread(double gamma, const ModelParams& p) {
  double sum = 0.0;
  for (int n = 0; n < p.steps; ++n) sum += optimal_spread(p.time_at(n), gamma, p);
  return sum / p.steps;
}

const char* strategy_name(Strategy s) noexcept {
  switch (s) {
    case Strategy::Inventory: return "inventory";
    case Strategy::Symmetric: return "symmetric";
    case Strategy::SymmetricTimeMatched: return "symmetric_tv";
  }
  return "unknown";
}

bool parse_strategy(const char* name, Strategy& out) noexcept {
  for (Strategy s : {Strategy::Inventory, Strategy::Symmetric, Strategy::SymmetricTimeMatched}) {
    if (std::strcmp(name, strategy_name(s)) == 0) {
      out = s;
      return true;
    }
  }
  return false;
}

QuotingRule::QuotingRule(Strategy strategy, double gamma, const ModelParams& p)
    : strategy_(strategy), gamma_(gamma), p_(p), average_spread_(average_optimal_spread(gamma, p)) {}

Quotes QuotingRule::operator()(int /*step*/, double t, double s, Qty q) const {
  switch (strategy_) {
    case Strategy::Inventory: {
      const double r = reservation_price(s, static_cast<double>(q), t, gamma_, p_);
      const double half = 0.5 * optimal_spread(t, gamma_, p_);
      return {r - half, r + half};
    }
    case Strategy::Symmetric:
      return {s - 0.5 * average_spread_, s + 0.5 * average_spread_};
    case Strategy::SymmetricTimeMatched: {
      const double half = 0.5 * optimal_spread(t, gamma_, p_);
      return {s - half, s + half};
    }
  }
  return {s, s};  // unreachable for valid enum values
}

void generate_path(Rng& rng, const ModelParams& p, Path& out) {
  const auto n = static_cast<std::size_t>(p.steps);
  out.mid.resize(n + 1);
  out.u_bid.resize(n);
  out.u_ask.resize(n);
  const double step_sd = p.sigma * std::sqrt(p.dt());
  out.mid[0] = p.s0;
  for (std::size_t i = 0; i < n; ++i) {
    out.mid[i + 1] = out.mid[i] + step_sd * rng.normal();
    out.u_bid[i] = rng.uniform();
    out.u_ask[i] = rng.uniform();
  }
}

void generate_path(std::uint64_t seed, std::uint64_t sim, const ModelParams& p, Path& out) {
  Rng rng = Rng::for_stream(seed, sim);
  generate_path(rng, p, out);
}

SimResult simulate(const Path& path, Strategy strategy, double gamma, const ModelParams& p,
                   std::vector<TraceRow>* trace) {
  return run_path(path, p, QuotingRule(strategy, gamma, p), trace);
}

}  // namespace lob::paper
