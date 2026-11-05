// Parameters of the backtest market simulator and of our quoting (units, defaults, rationale).
//
// UNITS used throughout lob::backtest (docs/design.md asks simulation code to state them):
//   price     integer ticks, 1 tick = $0.01 (kUsdPerTick); reference price 10,000 ticks = $100
//   quantity  the engine's Qty is SHARES (design.md convention); every simulated order is a whole
//             number of lots, 1 lot = 100 shares (kSharesPerLot), so 1 tick x 1 lot = $1
//   time      lob::Time nanoseconds inside the simulator; parameters are given in seconds
// All parameters are STYLIZED: chosen so that the simulated market is not degenerate (spread of a
// few ticks, a diffusive mid price), not estimated from or calibrated to any market data.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "lob/types.hpp"

namespace lob::backtest {

inline constexpr Qty kSharesPerLot = 100;
inline constexpr double kUsdPerTick = 0.01;
inline constexpr double kUsdPerTickLot = kUsdPerTick * static_cast<double>(kSharesPerLot);  // $1

// Id ranges. Background limit orders use 1, 2, 3, ... (creation count); market orders, which never
// rest, use kMarketIdBase + count; our orders use kOwnIdBase + count. The ranges never overlap.
inline constexpr OrderId kMarketIdBase = OrderId{1} << 61;
inline constexpr OrderId kOwnIdBase = OrderId{1} << 62;
inline bool is_own_id(OrderId id) noexcept { return id >= kOwnIdBase; }

struct SimParams {
  // ---- Episode timing ------------------------------------------------------------------------
  double horizon = 600.0;       // T, s: length of an episode
  double burn_in = 300.0;       // s of simulated market before the episode (not measured)
  double markout_tail = 10.0;   // s of market simulated after T, only to measure late markouts

  // ---- Background limit orders (zero-intelligence, Cont-Stoikov-Talreja style) ---------------
  std::int64_t ref_price = 10'000;  // ticks: initial price level ($100)
  double lo_rate = 1.0;             // lambda_0, 1/s per side: arrival rate at distance 1 tick
  double lo_alpha = 0.5;            // alpha: rate at distance i is lambda_0 * i^(-alpha)
  std::int64_t lo_levels = 10;      // K: distances 1..K ticks from the opposite best price
  double cancel_hazard = 0.3;       // theta, 1/s: each resting background order cancels at this rate
  double lo_size_mean = 2.0;        // lots: limit order size ~ 1 + Geometric, mean lo_size_mean
  std::int64_t init_levels = 10;    // initial book: levels per side at t = 0 (burn-in start)
  std::int64_t init_lots = 3;       // initial book: lots per level

  // ---- Background market orders --------------------------------------------------------------
  double mo_rate = 1.0;             // mu, 1/s: Poisson rate, side Bernoulli(1/2)
  double mo_size_mean = 2.0;        // lots: market order size ~ 1 + Geometric

  // ---- Informed flow (stress test): Hawkes clusters of same-signed market orders --------------
  double news_rate = 0.0;           // nu, 1/s: news (cluster immigrant) rate; 0 = no informed flow
  double branching = 0.5;           // n_br < 1: mean number of children per event
  double hawkes_decay = 2.0;        // beta, 1/s: exponential kernel decay (mean child delay 1/beta)
  double inf_size_mean = 2.0;       // lots: informed market order size ~ 1 + Geometric

  // ---- Our trading -------------------------------------------------------------------------------
  double latency = 0.001;           // s between a quoting decision and its effect on the book
  double requote = 0.1;             // s between quoting decisions
  std::int64_t max_inventory = 10;  // Q_max, lots
  std::int64_t quote_size = 1;      // lots per quote

  // ---- Checks ------------------------------------------------------------------------------------
  std::int64_t check_every = 0;     // call OrderBook::check_invariants every n events (0 = never)
};

// Description of one field, for the CLI, run.txt and the README parameter table.
struct ParamField {
  const char* name;      // CLI flag without the leading "--"
  const char* unit;
  const char* group;
  const char* rationale;  // one line
  double* d = nullptr;    // exactly one of d and i is set
  std::int64_t* i = nullptr;
};

std::vector<ParamField> param_fields(SimParams& p);

// Empty if the parameters are usable; otherwise a description of the first problem.
std::string validate(const SimParams& p);

// Seconds to nanoseconds, rounded to the nearest nanosecond.
Time seconds_to_ns(double s) noexcept;

}  // namespace lob::backtest
