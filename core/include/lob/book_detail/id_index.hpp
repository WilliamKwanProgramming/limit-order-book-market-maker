// Internal to lob::OrderBook: hash map from order id to pool slot. Not part of the public API.
//
// Open addressing with linear probing and backward-shift deletion, so there are no tombstones
// and lookups never degrade after many cancels. Keys are hashed with mix64 because exchange ids
// are often sequential, which would cluster badly under an identity hash. The table stays at most
// half full; it doubles (one allocation) when that limit would be exceeded, and reserve() sizes it
// up front so steady-state operation never allocates.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "lob/rng.hpp"  // mix64
#include "lob/types.hpp"

namespace lob::book_detail {

class IdIndex {
 public:
  static constexpr std::uint32_t kNotFound = UINT32_MAX;

  IdIndex() { rehash(kMinCapacity); }

  // Make room for `n` keys without further rehashing.
  void reserve(std::size_t n) {
    std::size_t cap = kMinCapacity;
    while (cap < 2 * n) cap *= 2;
    if (cap > slots_.size()) rehash(cap);
  }

  void clear() {
    for (Slot& s : slots_) s.key = kNoOrder;
    size_ = 0;
  }

  std::size_t size() const noexcept { return size_; }

  // Slot value stored for `id`, or kNotFound. `id` must not be kNoOrder.
  std::uint32_t find(OrderId id) const noexcept {
    for (std::size_t i = home(id);; i = (i + 1) & mask_) {
      const Slot& s = slots_[i];
      if (s.key == id) return s.value;
      if (s.key == kNoOrder) return kNotFound;
    }
  }

  // Insert a new key. Returns false (and changes nothing) if `id` is already present.
  bool insert(OrderId id, std::uint32_t value) {
    if (2 * (size_ + 1) > slots_.size()) rehash(2 * slots_.size());
    std::size_t i = home(id);
    for (; slots_[i].key != kNoOrder; i = (i + 1) & mask_) {
      if (slots_[i].key == id) return false;
    }
    slots_[i] = Slot{id, value};
    ++size_;
    return true;
  }

  // Remove a key. Returns false if it was not present.
  bool erase(OrderId id) noexcept {
    std::size_t i = home(id);
    while (slots_[i].key != id) {
      if (slots_[i].key == kNoOrder) return false;
      i = (i + 1) & mask_;
    }
    // Backward shift: pull later members of the probe run into the hole when their home slot
    // does not lie cyclically in (hole, j]; otherwise they would become unreachable.
    for (std::size_t j = (i + 1) & mask_; slots_[j].key != kNoOrder; j = (j + 1) & mask_) {
      const std::size_t h = home(slots_[j].key);
      const bool stays = (i < j) ? (i < h && h <= j) : (i < h || h <= j);
      if (!stays) {
        slots_[i] = slots_[j];
        i = j;
      }
    }
    slots_[i].key = kNoOrder;
    --size_;
    return true;
  }

  // Visit every (id, value) pair in unspecified order. Linear in capacity; for checks only.
  template <class F>
  void for_each(F&& f) const {
    for (const Slot& s : slots_) {
      if (s.key != kNoOrder) f(s.key, s.value);
    }
  }

 private:
  struct Slot {
    OrderId key = kNoOrder;
    std::uint32_t value = 0;
  };
  static constexpr std::size_t kMinCapacity = 16;

  std::size_t home(OrderId id) const noexcept {
    return static_cast<std::size_t>(mix64(id)) & mask_;
  }

  void rehash(std::size_t capacity) {
    std::vector<Slot> old(capacity);
    old.swap(slots_);
    mask_ = capacity - 1;
    size_ = 0;
    for (const Slot& s : old) {
      if (s.key != kNoOrder) insert(s.key, s.value);
    }
  }

  std::vector<Slot> slots_;
  std::size_t mask_ = 0;
  std::size_t size_ = 0;
};

}  // namespace lob::book_detail
