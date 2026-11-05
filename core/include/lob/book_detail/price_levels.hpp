// Internal to lob::OrderBook: the price levels of one side of the book. Not part of the public API.
//
// Two interchangeable containers with the same interface:
//
//   SortedVectorLevels  (default) contiguous std::vector sorted so that the BEST level is at the
//                       back. The best level is back(); removing it is pop_back(); new levels near
//                       the touch are inserted a few slots from the end, so only the handful of
//                       levels better than the new one move. Lookup scans a few levels from the
//                       best end, then falls back to binary search.
//   MapLevels           std::map keyed by price: O(log L) everything, one heap node per level.
//                       Kept only as the baseline for the benchmark in core/README.md; select it
//                       with the CMake option LOB_CORE_MAP_LEVELS.
//
// Both store a "key" instead of the price so one ordering serves both sides: key = price for bids
// and -price for asks, and a larger key is always a better price.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <vector>

#include "lob/types.hpp"

namespace lob::book_detail {

inline constexpr std::uint32_t kNil = UINT32_MAX;

// One price level: aggregate size and an intrusive FIFO of pool slots (oldest at head).
struct LevelQueue {
  Price price = 0;
  Qty qty = 0;
  std::uint32_t count = 0;
  std::uint32_t head = kNil;
  std::uint32_t tail = kNil;
};

class SortedVectorLevels {
 public:
  explicit SortedVectorLevels(bool is_bid) : is_bid_(is_bid) {}

  bool empty() const noexcept { return levels_.empty(); }
  std::size_t size() const noexcept { return levels_.size(); }
  void reserve(std::size_t n) { levels_.reserve(n); }
  void clear() noexcept { levels_.clear(); }

  // Preconditions: !empty().
  LevelQueue& best() noexcept { return levels_.back(); }
  const LevelQueue& best() const noexcept { return levels_.back(); }
  void pop_best() noexcept { levels_.pop_back(); }

  LevelQueue* find(Price price) noexcept {
    const std::size_t i = lower_index(key(price));
    return (i < levels_.size() && levels_[i].price == price) ? &levels_[i] : nullptr;
  }

  // Returns the level at `price`, creating an empty one if needed. May invalidate references to
  // other levels of this side.
  LevelQueue& find_or_insert(Price price) {
    const std::size_t i = lower_index(key(price));
    if (i < levels_.size() && levels_[i].price == price) return levels_[i];
    LevelQueue fresh;
    fresh.price = price;
    return *levels_.insert(levels_.begin() + static_cast<std::ptrdiff_t>(i), fresh);
  }

  // Precondition: a level at `price` exists.
  void erase(Price price) noexcept {
    const std::size_t i = lower_index(key(price));
    levels_.erase(levels_.begin() + static_cast<std::ptrdiff_t>(i));
  }

  // Visit levels best first; stop early when f returns false.
  template <class F>
  void for_each_best_first(F&& f) const {
    for (auto it = levels_.rbegin(); it != levels_.rend(); ++it) {
      if (!f(*it)) return;
    }
  }

 private:
  // Most traffic is within a few ticks of the touch, so a short linear scan from the best end
  // beats binary search there; deeper prices fall back to binary search.
  static constexpr std::size_t kLinearScan = 8;

  Price key(Price price) const noexcept { return is_bid_ ? price : -price; }

  // Index of the first level whose key is >= k (levels_ is sorted by ascending key).
  std::size_t lower_index(Price k) const noexcept {
    std::size_t i = levels_.size();
    const std::size_t stop = i > kLinearScan ? i - kLinearScan : 0;
    while (i > stop) {
      if (key(levels_[i - 1].price) < k) return i;
      --i;
    }
    if (i == 0) return 0;
    const auto it = std::lower_bound(
        levels_.begin(), levels_.begin() + static_cast<std::ptrdiff_t>(i), k,
        [this](const LevelQueue& lv, Price kk) { return key(lv.price) < kk; });
    return static_cast<std::size_t>(it - levels_.begin());
  }

  bool is_bid_;
  std::vector<LevelQueue> levels_;
};

class MapLevels {
 public:
  explicit MapLevels(bool is_bid) : is_bid_(is_bid) {}

  bool empty() const noexcept { return levels_.empty(); }
  std::size_t size() const noexcept { return levels_.size(); }
  void reserve(std::size_t /*n*/) {}
  void clear() noexcept { levels_.clear(); }

  LevelQueue& best() noexcept { return std::prev(levels_.end())->second; }
  const LevelQueue& best() const noexcept { return std::prev(levels_.end())->second; }
  void pop_best() { levels_.erase(std::prev(levels_.end())); }

  LevelQueue* find(Price price) noexcept {
    const auto it = levels_.find(key(price));
    return it == levels_.end() ? nullptr : &it->second;
  }

  LevelQueue& find_or_insert(Price price) {
    LevelQueue& lv = levels_[key(price)];
    lv.price = price;
    return lv;
  }

  void erase(Price price) { levels_.erase(key(price)); }

  template <class F>
  void for_each_best_first(F&& f) const {
    for (auto it = levels_.rbegin(); it != levels_.rend(); ++it) {
      if (!f(it->second)) return;
    }
  }

 private:
  Price key(Price price) const noexcept { return is_bid_ ? price : -price; }

  bool is_bid_;
  std::map<Price, LevelQueue> levels_;
};

#ifdef LOB_CORE_MAP_LEVELS
using PriceLevels = MapLevels;
#else
using PriceLevels = SortedVectorLevels;
#endif

}  // namespace lob::book_detail
