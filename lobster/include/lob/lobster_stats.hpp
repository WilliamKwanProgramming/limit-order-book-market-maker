// Small descriptive statistics of a replayed message stream (lobster_replay --stats).
//
// Call start(book) once before the first message (the book may already hold ghost orders), then
// observe(m, book) after each message has been applied.
//
//   counts per message type
//   executed volume per side: type 4 by the resting order's side; type 5 (hidden) by direction,
//     with directions other than +1/-1 counted apart
//   time-weighted means: the state after message k is weighted by t_{k+1} - t_k (zero for equal
//     timestamps; intervals with negative length are ignored, the state after the last message has
//     no weight). Spread = best ask - best bid in ticks, averaged over the time both sides are
//     non-empty; best bid size and best ask size, each averaged over the time that side is
//     non-empty.
//   best changes: messages after which the best bid or best ask differs from before the message,
//     counting a side appearing or disappearing; "price" compares best prices only, "quote" also
//     compares the sizes at the best prices.
#pragma once

#include <array>
#include <cstdint>

#include "lob/lobster_format.hpp"
#include "lob/order_book.hpp"

namespace lob::lobster {

struct StatsResult {
  std::uint64_t messages = 0;
  std::array<std::uint64_t, 8> by_type{};  // index = type
  Qty executed_bid = 0;  // type 4 against resting bids
  Qty executed_ask = 0;  // type 4 against resting asks
  Qty hidden_buy = 0;    // type 5, direction +1
  Qty hidden_sell = 0;   // type 5, direction -1
  Qty hidden_other = 0;  // type 5, any other direction
  double two_sided_seconds = 0.0;
  double mean_spread_ticks = 0.0;   // 0 if never two-sided for a positive time
  double mean_best_bid_qty = 0.0;
  double mean_best_ask_qty = 0.0;
  std::uint64_t best_price_changes = 0;
  std::uint64_t best_quote_changes = 0;
  double frac_best_price_changes() const noexcept {
    return messages ? static_cast<double>(best_price_changes) / static_cast<double>(messages) : 0.0;
  }
  double frac_best_quote_changes() const noexcept {
    return messages ? static_cast<double>(best_quote_changes) / static_cast<double>(messages) : 0.0;
  }
};

class Stats {
 public:
  void start(const OrderBook& book) noexcept;
  void observe(const Message& m, const OrderBook& book) noexcept;
  StatsResult result() const noexcept;

 private:
  struct Top {
    bool bid = false, ask = false;
    Price bid_px = 0, ask_px = 0;
    Qty bid_qty = 0, ask_qty = 0;
  };
  static Top top_of(const OrderBook& book) noexcept;

  StatsResult r_;
  Top top_;
  Time last_time_ = 0;
  bool have_time_ = false;
  double spread_ns_ = 0.0, two_sided_ns_ = 0.0;
  double bid_qty_ns_ = 0.0, bid_ns_ = 0.0;
  double ask_qty_ns_ = 0.0, ask_ns_ = 0.0;
};

}  // namespace lob::lobster
