// Statistical tests of the Hawkes cluster generator.
//
// Tolerances: every check is a z-test against the exact theoretical mean with |z| <= 5, using the
// theoretical (not the sample) variance. With a fixed seed the test is deterministic; for a fresh
// seed the chance of a false failure per check would be about 6e-7.
#include <cmath>
#include <map>

#include "lob/backtest/hawkes.hpp"
#include "lob/check.hpp"

using namespace lob;
using namespace lob::backtest;

namespace {

constexpr double kZ = 5.0;

void check_mean(double sample_mean, double mean, double var, double n, const char* what) {
  const double se = std::sqrt(var / n);
  const double z = (sample_mean - mean) / se;
  if (!(std::fabs(z) <= kZ)) {
    std::fprintf(stderr, "  %s: sample %.6g expected %.6g se %.3g z %.2f\n", what, sample_mean,
                 mean, se, z);
  }
  LOB_CHECK(std::fabs(z) <= kZ);
}

}  // namespace

LOB_TEST(mean_cluster_size_matches_galton_watson) {
  for (double n_br : {0.0, 0.3, 0.5, 0.8}) {
    HawkesParams p{1.0, n_br, 2.0};
    Rng rng = Rng::for_stream(101, static_cast<std::uint64_t>(n_br * 100));
    std::vector<HawkesEvent> ev;
    // Clusters are counted in full: the window is long and the last 200 s of immigrants (whose
    // descendants could be truncated at t1) are excluded.
    const double t1 = 100000.0;
    const std::uint32_t clusters = generate_hawkes(rng, p, 0.0, t1, ev);
    std::vector<double> size(clusters, 0.0), first_t(clusters, 0.0);
    for (const HawkesEvent& e : ev) {
      size[e.cluster] += 1.0;
      if (e.generation == 0) first_t[e.cluster] = e.t;
    }
    double sum = 0.0, n = 0.0;
    for (std::uint32_t c = 0; c < clusters; ++c) {
      if (first_t[c] > t1 - 200.0) continue;
      sum += size[c];
      n += 1.0;
    }
    const double mean = 1.0 / (1.0 - n_br);
    const double var = n_br / std::pow(1.0 - n_br, 3);  // total progeny, Poisson offspring
    if (n_br == 0.0) {
      LOB_CHECK_EQ(sum, n);
    } else {
      check_mean(sum / n, mean, var, n, "cluster size");
    }
  }
}

LOB_TEST(mean_intensity_is_nu_over_one_minus_n) {
  // Count over disjoint windows of length L >> 1/beta. For a stationary Hawkes process the count
  // variance over a long window is approximately nu L / (1 - n)^3; windows are nearly independent.
  for (double n_br : {0.5, 0.8}) {
    const HawkesParams p{0.2, n_br, 2.0};
    Rng rng = Rng::for_stream(202, static_cast<std::uint64_t>(n_br * 100));
    std::vector<HawkesEvent> ev;
    const double burn = 100.0, L = 200.0;
    const int windows = 2000;
    generate_hawkes(rng, p, 0.0, burn + L * windows, ev);
    std::vector<double> count(windows, 0.0);
    for (const HawkesEvent& e : ev) {
      if (e.t < burn) continue;
      const auto w = static_cast<std::size_t>((e.t - burn) / L);
      if (w < count.size()) count[w] += 1.0;
    }
    double sum = 0;
    for (double c : count) sum += c;
    const double mean = p.nu * L / (1.0 - n_br);
    const double var = p.nu * L / std::pow(1.0 - n_br, 3);
    check_mean(sum / windows, mean, var, windows, "window count");
  }
}

LOB_TEST(child_delays_are_exponential_beta) {
  const HawkesParams p{0.5, 0.6, 3.0};
  Rng rng(303);
  std::vector<HawkesEvent> ev;
  generate_hawkes(rng, p, 0.0, 50000.0, ev);
  // Within a cluster, the earliest child-generation event follows the immigrant; checking the
  // full parent-child delay needs the tree, so test the first-generation mean offset instead:
  // the mean time of generation-1 events after their immigrant is 1/beta.
  std::map<std::uint32_t, double> t0;
  for (const HawkesEvent& e : ev)
    if (e.generation == 0) t0[e.cluster] = e.t;
  double sum = 0, n = 0;
  for (const HawkesEvent& e : ev) {
    if (e.generation != 1) continue;
    sum += e.t - t0[e.cluster];
    n += 1;
  }
  check_mean(sum / n, 1.0 / p.beta, 1.0 / (p.beta * p.beta), n, "child delay");
}

LOB_TEST(clusters_are_same_signed_sorted_and_balanced) {
  const HawkesParams p{0.3, 0.7, 2.0};
  Rng rng(404);
  std::vector<HawkesEvent> ev;
  const std::uint32_t clusters = generate_hawkes(rng, p, 10.0, 20000.0, ev);
  std::map<std::uint32_t, Side> side;
  bool sorted = true, same = true, in_window = true;
  double buys = 0;
  for (std::size_t i = 0; i < ev.size(); ++i) {
    if (i > 0 && ev[i].t < ev[i - 1].t) sorted = false;
    if (ev[i].t < 10.0 || ev[i].t >= 20000.0) in_window = false;
    auto it = side.find(ev[i].cluster);
    if (it == side.end()) side[ev[i].cluster] = ev[i].side;
    else if (it->second != ev[i].side) same = false;
  }
  for (const auto& kv : side) buys += kv.second == Side::Bid ? 1.0 : 0.0;
  LOB_CHECK(sorted);
  LOB_CHECK(same);
  LOB_CHECK(in_window);
  LOB_CHECK_EQ(side.size(), static_cast<std::size_t>(clusters));
  const double n = static_cast<double>(clusters);
  check_mean(buys / n, 0.5, 0.25, n, "buy fraction");
  // Immigrant count is Poisson(nu * window).
  check_mean(n, p.nu * 19990.0, p.nu * 19990.0, 1.0, "cluster count");
}

LOB_TEST(no_news_no_events_and_deterministic) {
  Rng rng(5);
  std::vector<HawkesEvent> ev{{1.0, 0, 0, Side::Bid}};
  LOB_CHECK_EQ(generate_hawkes(rng, {0.0, 0.5, 1.0}, 0.0, 100.0, ev), 0u);
  LOB_CHECK(ev.empty());
  std::vector<HawkesEvent> a, b;
  Rng r1(9), r2(9);
  generate_hawkes(r1, {0.1, 0.8, 2.0}, 0.0, 5000.0, a);
  generate_hawkes(r2, {0.1, 0.8, 2.0}, 0.0, 5000.0, b);
  LOB_CHECK_EQ(a.size(), b.size());
  bool same = a.size() == b.size();
  for (std::size_t i = 0; same && i < a.size(); ++i)
    same = a[i].t == b[i].t && a[i].cluster == b[i].cluster && a[i].side == b[i].side;
  LOB_CHECK(same);
}

LOB_TEST_MAIN()
