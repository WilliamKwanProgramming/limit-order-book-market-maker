#include "lob/order_book.hpp"

#include <algorithm>
#include <sstream>

namespace lob {

using book_detail::kNil;
using book_detail::LevelQueue;

const char* to_string(Status s) noexcept {
  switch (s) {
    case Status::Ok: return "Ok";
    case Status::InvalidId: return "InvalidId";
    case Status::InvalidPrice: return "InvalidPrice";
    case Status::InvalidQty: return "InvalidQty";
    case Status::DuplicateId: return "DuplicateId";
    case Status::UnknownId: return "UnknownId";
  }
  return "?";
}

OrderBook::OrderBook() : levels_{book_detail::PriceLevels(true), book_detail::PriceLevels(false)} {}

void OrderBook::reserve(std::size_t max_orders, std::size_t max_levels_per_side) {
  pool_.reserve(max_orders);
  index_.reserve(max_orders);
  levels_[0].reserve(max_levels_per_side);
  levels_[1].reserve(max_levels_per_side);
}

void OrderBook::clear() {
  pool_.clear();
  free_head_ = kNil;
  index_.clear();
  levels_[0].clear();
  levels_[1].clear();
  total_qty_[0] = total_qty_[1] = 0;
}

// ---- Internals -----------------------------------------------------------------------------------

Status OrderBook::validate_new(OrderId id, Price price, Qty qty) noexcept {
  if (id == kNoOrder) return Status::InvalidId;
  if (price <= 0) return Status::InvalidPrice;
  if (qty <= 0) return Status::InvalidQty;
  return Status::Ok;
}

std::uint32_t OrderBook::allocate_node() {
  if (free_head_ != kNil) {
    const std::uint32_t n = free_head_;
    free_head_ = pool_[n].next;
    return n;
  }
  pool_.emplace_back();
  return static_cast<std::uint32_t>(pool_.size() - 1);
}

// Appends a new order at the back of its level. Caller has validated and checked for duplicates.
void OrderBook::rest(OrderId id, Side side, Price price, Qty qty, Time ts) {
  const std::uint32_t n = allocate_node();
  Node& node = pool_[n];
  node.id = id;
  node.side = side;
  node.price = price;
  node.qty = qty;
  node.ts = ts;
  node.next = kNil;

  LevelQueue& lv = levels(side).find_or_insert(price);
  node.prev = lv.tail;
  if (lv.tail != kNil) {
    pool_[lv.tail].next = n;
  } else {
    lv.head = n;
  }
  lv.tail = n;
  lv.qty += qty;
  ++lv.count;
  total_qty_[side_index(side)] += qty;
  index_.insert(id, n);
}

// Unlinks a node from its level (erasing the level if it empties), the index and the totals, and
// returns its slot to the free list.
void OrderBook::remove_node(std::uint32_t n) {
  Node& node = pool_[n];
  auto& side_levels = levels(node.side);
  LevelQueue* lv = side_levels.find(node.price);

  if (node.prev != kNil) {
    pool_[node.prev].next = node.next;
  } else {
    lv->head = node.next;
  }
  if (node.next != kNil) {
    pool_[node.next].prev = node.prev;
  } else {
    lv->tail = node.prev;
  }
  lv->qty -= node.qty;
  --lv->count;
  total_qty_[side_index(node.side)] -= node.qty;
  if (lv->count == 0) side_levels.erase(node.price);

  index_.erase(node.id);
  node.id = kNoOrder;
  node.next = free_head_;
  free_head_ = n;
}

// Reduces a resting order by 0 < by < remaining, keeping its queue position.
void OrderBook::shrink_node(std::uint32_t n, Qty by) {
  Node& node = pool_[n];
  node.qty -= by;
  levels(node.side).find(node.price)->qty -= by;
  total_qty_[side_index(node.side)] -= by;
}

// Trades an incoming order against the opposite side. Returns the size filled.
Qty OrderBook::match(OrderId aggressor, Side side, bool has_limit, Price limit, Qty qty, Time ts,
                     std::vector<Fill>& fills) {
  const Side resting_side = opposite(side);
  auto& book = levels(resting_side);
  Qty& side_total = total_qty_[side_index(resting_side)];
  Qty filled = 0;

  while (qty > 0 && !book.empty()) {
    LevelQueue& lv = book.best();
    if (has_limit && (side == Side::Bid ? lv.price > limit : lv.price < limit)) break;

    while (qty > 0 && lv.head != kNil) {
      const std::uint32_t n = lv.head;
      Node& node = pool_[n];
      const Qty take = std::min(qty, node.qty);
      node.qty -= take;
      lv.qty -= take;
      side_total -= take;
      qty -= take;
      filled += take;
      fills.push_back(Fill{node.id, aggressor, resting_side, lv.price, take, ts, node.qty});

      if (node.qty == 0) {
        // Fully filled head: unlink directly; the level is already in hand.
        lv.head = node.next;
        if (lv.head != kNil) {
          pool_[lv.head].prev = kNil;
        } else {
          lv.tail = kNil;
        }
        --lv.count;
        index_.erase(node.id);
        node.id = kNoOrder;
        node.next = free_head_;
        free_head_ = n;
      }
    }
    if (lv.count == 0) book.pop_best();
  }
  return filled;
}

// ---- Matching mode -------------------------------------------------------------------------------

SubmitResult OrderBook::submit_limit(OrderId id, Side side, Price price, Qty qty, Time ts,
                                     std::vector<Fill>& fills) {
  SubmitResult r;
  r.status = validate_new(id, price, qty);
  if (r.status != Status::Ok) return r;
  if (index_.find(id) != book_detail::IdIndex::kNotFound) {
    r.status = Status::DuplicateId;
    return r;
  }
  r.filled = match(id, side, true, price, qty, ts, fills);
  r.resting = qty - r.filled;
  if (r.resting > 0) rest(id, side, price, r.resting, ts);
  return r;
}

SubmitResult OrderBook::submit_market(OrderId id, Side side, Qty qty, Time ts,
                                      std::vector<Fill>& fills) {
  SubmitResult r;
  if (id == kNoOrder) {
    r.status = Status::InvalidId;
  } else if (qty <= 0) {
    r.status = Status::InvalidQty;
  } else if (index_.find(id) != book_detail::IdIndex::kNotFound) {
    r.status = Status::DuplicateId;
  } else {
    r.filled = match(id, side, false, 0, qty, ts, fills);
  }
  return r;
}

SubmitResult OrderBook::replace(OrderId id, Price new_price, Qty new_qty, Time ts,
                                std::vector<Fill>& fills) {
  SubmitResult r;
  r.status = validate_new(id, new_price, new_qty);
  if (r.status != Status::Ok) return r;
  const std::uint32_t n = index_.find(id);
  if (n == book_detail::IdIndex::kNotFound) {
    r.status = Status::UnknownId;
    return r;
  }
  const Node& node = pool_[n];
  if (new_price == node.price && new_qty <= node.qty) {
    if (new_qty < node.qty) shrink_node(n, node.qty - new_qty);
    r.resting = new_qty;
    return r;
  }
  // Loses priority: equivalent to cancel followed by a fresh limit order with the same id.
  const Side side = node.side;
  remove_node(n);
  return submit_limit(id, side, new_price, new_qty, ts, fills);
}

// ---- Both modes ----------------------------------------------------------------------------------

Status OrderBook::cancel(OrderId id) {
  if (id == kNoOrder) return Status::InvalidId;
  const std::uint32_t n = index_.find(id);
  if (n == book_detail::IdIndex::kNotFound) return Status::UnknownId;
  remove_node(n);
  return Status::Ok;
}

Status OrderBook::reduce(OrderId id, Qty by_qty) {
  if (id == kNoOrder) return Status::InvalidId;
  if (by_qty <= 0) return Status::InvalidQty;
  const std::uint32_t n = index_.find(id);
  if (n == book_detail::IdIndex::kNotFound) return Status::UnknownId;
  if (by_qty >= pool_[n].qty) {
    remove_node(n);
  } else {
    shrink_node(n, by_qty);
  }
  return Status::Ok;
}

// ---- Replay mode ---------------------------------------------------------------------------------

Status OrderBook::add_resting(OrderId id, Side side, Price price, Qty qty, Time ts) {
  const Status s = validate_new(id, price, qty);
  if (s != Status::Ok) return s;
  if (index_.find(id) != book_detail::IdIndex::kNotFound) return Status::DuplicateId;
  rest(id, side, price, qty, ts);
  return Status::Ok;
}

Status OrderBook::execute_resting(OrderId id, Qty qty, Time ts, std::vector<Fill>& fills) {
  if (id == kNoOrder) return Status::InvalidId;
  if (qty <= 0) return Status::InvalidQty;
  const std::uint32_t n = index_.find(id);
  if (n == book_detail::IdIndex::kNotFound) return Status::UnknownId;
  const Node& node = pool_[n];
  const Qty take = std::min(qty, node.qty);
  fills.push_back(Fill{id, kNoOrder, node.side, node.price, take, ts, node.qty - take});
  if (take == node.qty) {
    remove_node(n);
  } else {
    shrink_node(n, take);
  }
  return Status::Ok;
}

// ---- Queries -------------------------------------------------------------------------------------

std::optional<double> OrderBook::mid() const noexcept {
  if (levels_[0].empty() || levels_[1].empty()) return std::nullopt;
  // Sum in floating point: two large prices could overflow an integer sum.
  return 0.5 * (static_cast<double>(levels_[0].best().price) +
                static_cast<double>(levels_[1].best().price));
}

std::optional<Price> OrderBook::spread() const noexcept {
  if (levels_[0].empty() || levels_[1].empty()) return std::nullopt;
  return levels_[1].best().price - levels_[0].best().price;
}

std::optional<OrderInfo> OrderBook::front(Side side) const noexcept {
  const auto& lv = levels(side);
  if (lv.empty()) return std::nullopt;
  const Node& node = pool_[lv.best().head];
  return OrderInfo{node.id, node.side, node.price, node.qty, node.ts};
}

void OrderBook::depth(Side side, std::size_t n, std::vector<Level>& out) const {
  out.clear();
  if (n == 0) return;
  levels(side).for_each_best_first([&](const LevelQueue& lv) {
    out.push_back(Level{lv.price, lv.qty, lv.count});
    return out.size() < n;
  });
}

std::optional<OrderInfo> OrderBook::find(OrderId id) const noexcept {
  if (id == kNoOrder) return std::nullopt;
  const std::uint32_t n = index_.find(id);
  if (n == book_detail::IdIndex::kNotFound) return std::nullopt;
  const Node& node = pool_[n];
  return OrderInfo{node.id, node.side, node.price, node.qty, node.ts};
}

std::optional<Qty> OrderBook::queue_ahead(OrderId id) const noexcept {
  if (id == kNoOrder) return std::nullopt;
  const std::uint32_t n = index_.find(id);
  if (n == book_detail::IdIndex::kNotFound) return std::nullopt;
  Qty ahead = 0;
  for (std::uint32_t p = pool_[n].prev; p != kNil; p = pool_[p].prev) ahead += pool_[p].qty;
  return ahead;
}

// ---- Checks --------------------------------------------------------------------------------------

bool OrderBook::check_invariants(bool require_uncrossed, std::string* why) const {
  std::ostringstream err;
  std::size_t linked = 0;
  bool ok = true;
  auto fail = [&](const char* what, long long detail) {
    if (ok) err << what << " (" << detail << ")";
    ok = false;
  };

  for (const Side side : {Side::Bid, Side::Ask}) {
    Qty side_qty = 0;
    bool first = true;
    Price prev_price = 0;
    levels(side).for_each_best_first([&](const LevelQueue& lv) {
      if (!first && (side == Side::Bid ? lv.price >= prev_price : lv.price <= prev_price)) {
        fail("levels not strictly sorted from best at price", lv.price);
      }
      first = false;
      prev_price = lv.price;
      if (lv.count == 0 || lv.qty <= 0) fail("empty level at price", lv.price);

      Qty qty = 0;
      std::uint32_t count = 0;
      std::uint32_t prev = kNil;
      for (std::uint32_t n = lv.head; n != kNil; n = pool_[n].next) {
        if (n >= pool_.size() || count > pool_.size()) {  // dangling index or cycle
          fail("broken queue link at price", lv.price);
          break;
        }
        const Node& node = pool_[n];
        if (node.prev != prev) fail("prev link mismatch for order", static_cast<long long>(node.id));
        if (node.side != side || node.price != lv.price) {
          fail("order on wrong level", static_cast<long long>(node.id));
        }
        if (node.qty <= 0) fail("non-positive order size", static_cast<long long>(node.id));
        if (index_.find(node.id) != n) fail("index mismatch for order", static_cast<long long>(node.id));
        qty += node.qty;
        ++count;
        prev = n;
      }
      if (lv.tail != prev) fail("tail mismatch at price", lv.price);
      if (qty != lv.qty) fail("level size != sum of orders at price", lv.price);
      if (count != lv.count) fail("level count != number of orders at price", lv.price);
      side_qty += qty;
      linked += count;
      return true;
    });
    if (side_qty != total_qty_[side_index(side)]) fail("side total mismatch", side_qty);
  }

  if (linked != index_.size()) fail("index size != linked orders", static_cast<long long>(linked));
  std::size_t free_count = 0;
  for (std::uint32_t n = free_head_; n != kNil && free_count <= pool_.size(); n = pool_[n].next) {
    if (pool_[n].id != kNoOrder) fail("live node on free list", n);
    ++free_count;
  }
  if (linked + free_count != pool_.size()) {
    fail("pool slots leaked or double-freed", static_cast<long long>(pool_.size()));
  }
  if (require_uncrossed && !levels_[0].empty() && !levels_[1].empty() &&
      levels_[0].best().price >= levels_[1].best().price) {
    fail("book locked or crossed, bid", levels_[0].best().price);
  }

  if (!ok && why) *why = err.str();
  return ok;
}

}  // namespace lob
