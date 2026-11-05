// Basic types shared by every module. Conventions are described in docs/design.md.
#pragma once

#include <cmath>
#include <cstdint>

namespace lob {

using OrderId = std::uint64_t;

// Price in integer ticks. Replay code uses the LOBSTER convention: 1 tick = 0.0001 USD, so the
// price 5859400 is $585.94. Simulation code may choose another tick size but must say so.
using Price = std::int64_t;

// Quantity in shares. Signed so that order sizes, deltas and inventory share one type.
using Qty = std::int64_t;

// Time in nanoseconds: since midnight for replay, since the start of the run for simulation.
using Time = std::int64_t;

inline constexpr Time kNanosPerSecond = 1'000'000'000;
inline constexpr double kUsdPerLobsterTick = 1e-4;

// "No order": for example the external aggressor of an execution reported by LOBSTER.
// LOBSTER order ids are positive, so 0 is free to use as a sentinel.
inline constexpr OrderId kNoOrder = 0;

// Bid = +1 and Ask = -1 match LOBSTER's direction field (1 = buy limit order, -1 = sell limit order).
enum class Side : std::int8_t { Bid = 1, Ask = -1 };

constexpr Side opposite(Side s) noexcept { return s == Side::Bid ? Side::Ask : Side::Bid; }
constexpr int sign(Side s) noexcept { return static_cast<int>(s); }

inline double ticks_to_usd(Price p) noexcept { return static_cast<double>(p) * kUsdPerLobsterTick; }
inline Price usd_to_ticks(double usd) noexcept {
  return static_cast<Price>(std::llround(usd / kUsdPerLobsterTick));
}

}  // namespace lob
