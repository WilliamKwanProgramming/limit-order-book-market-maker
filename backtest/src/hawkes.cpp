#include "lob/backtest/hawkes.hpp"

#include <algorithm>
#include <tuple>

namespace lob::backtest {

std::uint32_t generate_hawkes(Rng& rng, const HawkesParams& p, double t0, double t1,
                              std::vector<HawkesEvent>& out) {
  out.clear();
  if (!(p.nu > 0.0) || !(t1 > t0)) return 0;
  std::uint32_t clusters = 0;
  double t = t0;
  std::size_t first = 0;
  for (;;) {
    t += rng.exponential(p.nu);
    if (t >= t1) break;
    const Side side = rng.bernoulli(0.5) ? Side::Bid : Side::Ask;
    first = out.size();
    out.push_back({t, clusters, 0, side});
    // Breadth first: out[first..] grows while we walk it.
    for (std::size_t j = first; j < out.size(); ++j) {
      const HawkesEvent parent = out[j];
      const std::uint64_t children = p.n_br > 0.0 ? rng.poisson(p.n_br) : 0;
      for (std::uint64_t c = 0; c < children; ++c) {
        const double tc = parent.t + rng.exponential(p.beta);
        if (tc < t1) out.push_back({tc, clusters, parent.generation + 1, side});
      }
    }
    ++clusters;
  }
  std::sort(out.begin(), out.end(), [](const HawkesEvent& a, const HawkesEvent& b) {
    return std::tie(a.t, a.cluster, a.generation) < std::tie(b.t, b.cluster, b.generation);
  });
  return clusters;
}

}  // namespace lob::backtest
