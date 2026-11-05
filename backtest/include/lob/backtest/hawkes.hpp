// Informed order flow: a marked Hawkes process simulated exactly through its cluster
// (immigrant/offspring, Galton-Watson) representation.
//
// News events (immigrants) arrive as a Poisson process of rate nu on [t0, t1); each has a random
// direction (buy or sell with probability 1/2). Every event, immigrant or child, has
// Poisson(n_br) children, each delayed from its parent by an independent Exponential(beta) time.
// All events of a cluster carry the immigrant's direction. Events at or after t1 are dropped
// together with their descendants (which would also fall after t1).
//
// The unmarked event count is then a Hawkes process with baseline nu and kernel
// n_br * beta * exp(-beta s): stationary mean intensity nu / (1 - n_br), mean cluster size
// 1 / (1 - n_br), cluster-size variance n_br / (1 - n_br)^3 (Poisson offspring).
#pragma once

#include <cstdint>
#include <vector>

#include "lob/rng.hpp"
#include "lob/types.hpp"

namespace lob::backtest {

struct HawkesParams {
  double nu = 0.0;      // immigrant rate, 1/s
  double n_br = 0.5;    // branching ratio, 0 <= n_br < 1
  double beta = 1.0;    // kernel decay, 1/s
};

struct HawkesEvent {
  double t = 0.0;               // seconds
  std::uint32_t cluster = 0;    // index of the immigrant, in arrival order
  std::uint32_t generation = 0; // 0 for the immigrant
  Side side = Side::Bid;        // aggressor side: Bid = buy market order (lifts asks)
};

// Appends the events of all clusters whose immigrant falls in [t0, t1) to `out` (cleared first),
// sorted by time (ties by cluster, then generation). Draws only from `rng`, in a fixed order:
// per immigrant its interarrival time and direction, then its descendants breadth first.
// Returns the number of clusters.
std::uint32_t generate_hawkes(Rng& rng, const HawkesParams& p, double t0, double t1,
                              std::vector<HawkesEvent>& out);

}  // namespace lob::backtest
