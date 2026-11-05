// Avellaneda and Stoikov (2008), "High-frequency trading in a limit order book",
// Quantitative Finance 8(3), 217-224: model formulas and the discrete-time simulation of their
// Section 4 ("Numerical simulations").
//
// Simulation step n (time t_n = n * dt), in the order the paper describes:
//   1. the agent quotes bid and ask from the state (s_n, q_n, t_n);
//   2. the ask is lifted with probability min(1, lambda(delta_a) dt) and the bid is hit with
//      probability min(1, lambda(delta_b) dt), independently; both may happen in one step;
//   3. the mid moves: s_{n+1} = s_n + sigma sqrt(dt) Z_n with Z_n standard normal.
// Profit is cash plus inventory marked at the final mid: X_T + q_T S_T (initial cash and
// inventory are zero).
#pragma once

#include <cstdint>
#include <vector>

#include "lob/rng.hpp"
#include "lob/types.hpp"

namespace lob::paper {

// Parameters of the paper's simulation. Prices are in dollars (continuous, no tick grid).
struct ModelParams {
  double s0 = 100.0;    // initial mid price
  double sigma = 2.0;   // volatility of the mid (arithmetic Brownian motion)
  double horizon = 1.0; // T
  double A = 140.0;     // arrival intensity at zero distance
  double k = 1.5;       // decay of arrival intensity with distance
  int steps = 200;      // N, so dt = T / N = 0.005

  double dt() const noexcept { return horizon / steps; }
  double time_at(int n) const noexcept { return horizon * n / steps; }
};

// Indifference (reservation) price r(s, q, t) = s - q gamma sigma^2 (T - t).
double reservation_price(double s, double q, double t, double gamma, const ModelParams& p);

// Total quoted spread delta_a + delta_b = gamma sigma^2 (T - t) + (2 / gamma) ln(1 + gamma / k).
// It does not depend on inventory or on the mid. Requires gamma > 0.
double optimal_spread(double t, double gamma, const ModelParams& p);

// Probability that a quote at signed distance `delta` from the mid fills within one step:
// min(1, A exp(-k delta) dt). A negative distance (a quote through the mid) is allowed; its
// intensity exceeds A and the probability is capped at 1.
double fill_probability(double delta, const ModelParams& p);

// Time average of optimal_spread over the N quoting times t_0 .. t_{N-1}: exactly the average
// spread the inventory strategy quotes in the simulation.
double average_optimal_spread(double gamma, const ModelParams& p);

enum class Strategy : std::uint8_t {
  // Quotes r +/- spread(t) / 2 around the reservation price (the paper's "inventory" strategy).
  Inventory,
  // The paper's benchmark: mid +/- s_bar / 2 with s_bar the inventory strategy's average spread.
  Symmetric,
  // Variant: mid +/- spread(t) / 2, the same spread as the inventory strategy at every step.
  SymmetricTimeMatched,
};

const char* strategy_name(Strategy s) noexcept;
// Parses a name produced by strategy_name. Returns false if the name is unknown.
bool parse_strategy(const char* name, Strategy& out) noexcept;

struct Quotes {
  double bid;
  double ask;
};

// Quoting rule of a named strategy, with its time-independent parts computed once.
class QuotingRule {
 public:
  QuotingRule(Strategy strategy, double gamma, const ModelParams& p);
  Quotes operator()(int step, double t, double s, Qty q) const;
  double symmetric_spread() const noexcept { return average_spread_; }

 private:
  Strategy strategy_;
  double gamma_;
  ModelParams p_;
  double average_spread_;  // average_optimal_spread(gamma, p), used by Symmetric
};

// The random inputs of one simulation. Every strategy (and every gamma) run on the same Path sees
// the same mid-price path and the same fill uniforms, which makes comparisons paired.
struct Path {
  std::vector<double> mid;    // s_0 .. s_N (N + 1 values)
  std::vector<double> u_bid;  // N uniforms on [0, 1): bid fills at step n iff u_bid[n] < P(fill)
  std::vector<double> u_ask;  // N uniforms on [0, 1), likewise for the ask
};

// Draws a path from `rng`: per step one normal, then the bid uniform, then the ask uniform.
void generate_path(Rng& rng, const ModelParams& p, Path& out);

// Path of simulation `sim` under `seed`: generate_path with Rng::for_stream(seed, sim).
void generate_path(std::uint64_t seed, std::uint64_t sim, const ModelParams& p, Path& out);

struct SimResult {
  double profit = 0.0;      // cash + final_q * final_mid
  double cash = 0.0;
  Qty final_q = 0;
  double final_mid = 0.0;
  double mean_spread = 0.0; // average of (ask - bid) over the N steps
  int bid_fills = 0;
  int ask_fills = 0;
};

// One row per step of a traced simulation: the state when quoting and what happened.
struct TraceRow {
  double t;
  double mid;
  double bid;
  double ask;
  Qty q;          // inventory before this step's fills
  double cash;    // cash before this step's fills
  bool bid_fill;
  bool ask_fill;
};

// Runs one simulation on `path` with an arbitrary quoting rule
// quote(int step, double t, double s, Qty q) -> Quotes. Appends to `trace` when it is non-null.
template <class QuoteFn>
SimResult run_path(const Path& path, const ModelParams& p, QuoteFn&& quote,
                   std::vector<TraceRow>* trace = nullptr) {
  SimResult r;
  Qty q = 0;
  double cash = 0.0;
  double spread_sum = 0.0;
  for (int n = 0; n < p.steps; ++n) {
    const auto i = static_cast<std::size_t>(n);
    const double t = p.time_at(n);
    const double s = path.mid[i];
    const Quotes qt = quote(n, t, s, q);
    spread_sum += qt.ask - qt.bid;
    const bool bid_fill = path.u_bid[i] < fill_probability(s - qt.bid, p);
    const bool ask_fill = path.u_ask[i] < fill_probability(qt.ask - s, p);
    if (trace) trace->push_back({t, s, qt.bid, qt.ask, q, cash, bid_fill, ask_fill});
    // Fills execute at the quoted prices. If both fill, inventory is unchanged and cash gains
    // the quoted spread.
    if (bid_fill) {
      q += 1;
      cash -= qt.bid;
      ++r.bid_fills;
    }
    if (ask_fill) {
      q -= 1;
      cash += qt.ask;
      ++r.ask_fills;
    }
  }
  r.cash = cash;
  r.final_q = q;
  r.final_mid = path.mid[static_cast<std::size_t>(p.steps)];
  r.profit = cash + static_cast<double>(q) * r.final_mid;
  r.mean_spread = spread_sum / p.steps;
  return r;
}

// Runs one simulation of a named strategy.
SimResult simulate(const Path& path, Strategy strategy, double gamma, const ModelParams& p,
                   std::vector<TraceRow>* trace = nullptr);

}  // namespace lob::paper
