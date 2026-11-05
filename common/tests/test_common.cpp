#include <cmath>
#include <cstdint>
#include <vector>

#include "lob/check.hpp"
#include "lob/rng.hpp"
#include "lob/types.hpp"

using namespace lob;

// Reference values come from an independent Python implementation of SplitMix64 seeding and
// xoshiro256++ (see docs/design.md), so these tests pin the exact stream.
LOB_TEST(rng_matches_reference_stream_seed_1) {
  Rng r(1);
  LOB_CHECK_EQ(r.next_u64(), 0xCFC5D07F6F03C29BULL);
  LOB_CHECK_EQ(r.next_u64(), 0xBF424132963FE08DULL);
  LOB_CHECK_EQ(r.next_u64(), 0x19A37D5757AAF520ULL);
  LOB_CHECK_EQ(r.next_u64(), 0xBF08119F05CD56D6ULL);
}

LOB_TEST(rng_matches_reference_stream_seed_42) {
  Rng r(42);
  LOB_CHECK_EQ(r.next_u64(), 0xD0764D4F4476689FULL);
  LOB_CHECK_EQ(r.next_u64(), 0x519E4174576F3791ULL);
  LOB_CHECK_EQ(r.next_u64(), 0xFBE07CFB0C24ED8CULL);
  LOB_CHECK_EQ(r.next_u64(), 0xB37D9F600CD835B8ULL);
}

LOB_TEST(uniform_matches_reference) {
  Rng r(1);
  LOB_CHECK_EQ(r.uniform(), 0.81161215888188476);
  LOB_CHECK_EQ(r.uniform(), 0.74710471615821872);
  LOB_CHECK_EQ(r.uniform(), 0.10015090353378375);
}

LOB_TEST(normal_matches_reference) {
  Rng r(7);
  LOB_CHECK_NEAR(r.normal(), 1.1308649617728408, 1e-12);
  LOB_CHECK_NEAR(r.normal(), 2.1234228511806617, 1e-12);
}

LOB_TEST(reseed_restarts_the_stream) {
  Rng r(5);
  const std::uint64_t first = r.next_u64();
  r.next_u64();
  r.reseed(5);
  LOB_CHECK_EQ(r.next_u64(), first);
}

LOB_TEST(uniform_ranges) {
  Rng r(3);
  for (int i = 0; i < 100000; ++i) {
    const double u = r.uniform();
    const double o = r.uniform_open();
    LOB_CHECK(u >= 0.0 && u < 1.0);
    LOB_CHECK(o > 0.0 && o < 1.0);
  }
}

LOB_TEST(uniform_moments) {
  Rng r(2);
  const int n = 1000000;
  double s = 0, s2 = 0;
  for (int i = 0; i < n; ++i) {
    const double x = r.uniform();
    s += x;
    s2 += x * x;
  }
  const double mean = s / n;
  LOB_CHECK_NEAR(mean, 0.5, 3e-3);
  LOB_CHECK_NEAR(s2 / n - mean * mean, 1.0 / 12.0, 2e-3);
}

LOB_TEST(uniform_int_is_in_range_and_unbiased) {
  Rng r(4);
  const std::uint64_t buckets = 7;
  std::vector<long> count(buckets, 0);
  const long n = 700000;
  for (long i = 0; i < n; ++i) ++count[r.uniform_int(buckets)];
  for (long c : count) LOB_CHECK_NEAR(c, n / 7, 1500);  // about 5 standard deviations

  for (int i = 0; i < 10000; ++i) {
    const std::int64_t v = r.uniform_int(std::int64_t{-3}, std::int64_t{3});
    LOB_CHECK(v >= -3 && v <= 3);
  }
}

LOB_TEST(normal_moments) {
  Rng r(6);
  const int n = 1000000;
  double s = 0, s2 = 0;
  long within_one_sd = 0;
  for (int i = 0; i < n; ++i) {
    const double x = r.normal();
    s += x;
    s2 += x * x;
    if (std::fabs(x) < 1.0) ++within_one_sd;
  }
  const double mean = s / n;
  LOB_CHECK_NEAR(mean, 0.0, 6e-3);
  LOB_CHECK_NEAR(s2 / n - mean * mean, 1.0, 1e-2);
  LOB_CHECK_NEAR(static_cast<double>(within_one_sd) / n, 0.6826894921, 5e-3);
}

LOB_TEST(exponential_mean) {
  Rng r(8);
  const int n = 1000000;
  double s = 0;
  for (int i = 0; i < n; ++i) s += r.exponential(2.5);
  LOB_CHECK_NEAR(s / n, 0.4, 3e-3);
}

LOB_TEST(poisson_mean_and_variance) {
  Rng r(11);
  for (double mean : {0.3, 4.0, 29.0, 100.0}) {
    const int n = 200000;
    double s = 0, s2 = 0;
    for (int i = 0; i < n; ++i) {
      const auto x = static_cast<double>(r.poisson(mean));
      s += x;
      s2 += x * x;
    }
    const double m = s / n;
    LOB_CHECK_NEAR(m, mean, 0.02 * mean + 0.01);
    LOB_CHECK_NEAR(s2 / n - m * m, mean, 0.05 * mean + 0.02);
  }
}

LOB_TEST(bernoulli_rate) {
  Rng r(12);
  const int n = 1000000;
  long hits = 0;
  for (int i = 0; i < n; ++i) hits += r.bernoulli(0.3) ? 1 : 0;
  LOB_CHECK_NEAR(static_cast<double>(hits) / n, 0.3, 3e-3);
}

LOB_TEST(streams_are_reproducible_and_distinct) {
  Rng a = Rng::for_stream(1, 0);
  Rng b = Rng::for_stream(1, 0);
  Rng c = Rng::for_stream(1, 1);
  Rng d = Rng::for_stream(2, 0);
  const std::uint64_t first = a.next_u64();
  LOB_CHECK_EQ(first, b.next_u64());
  LOB_CHECK(first != c.next_u64());
  LOB_CHECK(first != d.next_u64());
}

LOB_TEST(side_and_price_helpers) {
  LOB_CHECK(opposite(Side::Bid) == Side::Ask);
  LOB_CHECK(opposite(Side::Ask) == Side::Bid);
  LOB_CHECK_EQ(sign(Side::Bid), 1);
  LOB_CHECK_EQ(sign(Side::Ask), -1);
  LOB_CHECK_EQ(usd_to_ticks(585.94), Price{5859400});
  LOB_CHECK_NEAR(ticks_to_usd(5859400), 585.94, 1e-9);
}

LOB_TEST_MAIN()
