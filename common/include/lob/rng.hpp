// Deterministic random numbers: xoshiro256++ (Blackman and Vigna) seeded through SplitMix64.
//
// The distribution transforms live here instead of using <random> distributions, because the
// standard leaves their algorithms implementation-defined. The raw bit stream is identical on
// every platform; transforms that call libm (log, sin, cos) can differ in the last ulp between
// C libraries, so cross-platform reproducibility is statistical rather than bit-for-bit.
#pragma once

#include <cmath>
#include <cstdint>

namespace lob {

inline constexpr double kPi = 3.14159265358979323846;

// SplitMix64 finalizer: a strong 64-bit mixing function.
constexpr std::uint64_t mix64(std::uint64_t z) noexcept {
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

class Rng {
 public:
  explicit Rng(std::uint64_t seed = 1) noexcept { reseed(seed); }

  // Independent generator number `stream_id` derived from `seed`, for per-component randomness.
  static Rng for_stream(std::uint64_t seed, std::uint64_t stream_id) noexcept {
    return Rng(mix64(seed + 0x9E3779B97F4A7C15ULL) ^ mix64(stream_id * 0xD1B54A32D192ED03ULL + 1));
  }

  void reseed(std::uint64_t seed) noexcept {
    std::uint64_t x = seed;
    for (auto& w : s_) {
      x += 0x9E3779B97F4A7C15ULL;
      w = mix64(x);
    }
    have_spare_ = false;
  }

  // 64 random bits.
  std::uint64_t next_u64() noexcept {
    const std::uint64_t result = rotl(s_[0] + s_[3], 23) + s_[0];
    const std::uint64_t t = s_[1] << 17;
    s_[2] ^= s_[0];
    s_[3] ^= s_[1];
    s_[1] ^= s_[2];
    s_[0] ^= s_[3];
    s_[2] ^= t;
    s_[3] = rotl(s_[3], 45);
    return result;
  }

  // Uniform on [0, 1) with 53 bits of resolution.
  double uniform() noexcept { return static_cast<double>(next_u64() >> 11) * 0x1.0p-53; }

  // Uniform on the open interval (0, 1): never 0, so std::log is always finite.
  double uniform_open() noexcept {
    return (static_cast<double>(next_u64() >> 11) + 0.5) * 0x1.0p-53;
  }

  double uniform(double lo, double hi) noexcept { return lo + (hi - lo) * uniform(); }

  // Unbiased integer in [0, n) by rejection of the biased tail. Requires n > 0.
  std::uint64_t uniform_int(std::uint64_t n) noexcept {
    const std::uint64_t threshold = (0ULL - n) % n;  // 2^64 mod n
    std::uint64_t r = next_u64();
    while (r < threshold) r = next_u64();
    return r % n;
  }

  // Unbiased integer in [lo, hi], both inclusive. Requires lo <= hi.
  std::int64_t uniform_int(std::int64_t lo, std::int64_t hi) noexcept {
    const auto span = static_cast<std::uint64_t>(hi) - static_cast<std::uint64_t>(lo) + 1ULL;
    if (span == 0) return static_cast<std::int64_t>(next_u64());  // full 64-bit range
    return static_cast<std::int64_t>(static_cast<std::uint64_t>(lo) + uniform_int(span));
  }

  bool bernoulli(double p) noexcept { return uniform() < p; }

  // Standard normal by Box-Muller; the second variate of each pair is cached.
  double normal() noexcept {
    if (have_spare_) {
      have_spare_ = false;
      return spare_;
    }
    const double u1 = uniform_open();
    const double u2 = uniform();
    const double r = std::sqrt(-2.0 * std::log(u1));
    const double theta = 2.0 * kPi * u2;
    spare_ = r * std::sin(theta);
    have_spare_ = true;
    return r * std::cos(theta);
  }

  double normal(double mean, double sd) noexcept { return mean + sd * normal(); }

  // Exponential with the given rate (mean 1 / rate). Requires rate > 0.
  double exponential(double rate) noexcept { return -std::log(uniform_open()) / rate; }

  // Poisson with the given mean >= 0. Exact: sums independent Poisson(<= 30) pieces, each drawn by
  // Knuth's multiplication method.
  std::uint64_t poisson(double mean) noexcept {
    std::uint64_t total = 0;
    while (mean > 30.0) {
      total += poisson_small(30.0);
      mean -= 30.0;
    }
    return total + poisson_small(mean);
  }

 private:
  static constexpr std::uint64_t rotl(std::uint64_t x, int k) noexcept {
    return (x << k) | (x >> (64 - k));
  }

  std::uint64_t poisson_small(double mean) noexcept {
    const double limit = std::exp(-mean);
    std::uint64_t k = 0;
    double p = uniform();
    while (p > limit) {
      ++k;
      p *= uniform();
    }
    return k;
  }

  std::uint64_t s_[4];
  double spare_ = 0.0;
  bool have_spare_ = false;
};

}  // namespace lob
