// A deliberately naive order book used as the oracle in the differential tests.
//
// It is written for obviousness, not speed: std::map of price -> std::deque of orders per side,
// linear scans for cancels, and no shared code with lob::OrderBook beyond the public value types.
// It implements the contract in docs/design.md directly, including the status codes and the
// replace priority rule, so any disagreement with the engine is a bug in one of the two.
#pragma once

#include <algorithm>
#include <deque>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "lob/order_book.hpp"

namespace lob::testing {

class ReferenceBook {
 public:
  struct Order {
    OrderId id;
    Qty qty;
    Time ts;
  };
  using Queue = std::deque<Order>;
  using SideMap = std::map<Price, Queue>;  // ascending price on both sides

  SubmitResult submit_limit(OrderId id, Side side, Price price, Qty qty, Time ts,
                            std::vector<Fill>& fills) {
    SubmitResult r;
    r.status = validate(id, price, qty);
    if (r.status == Status::Ok && find(id)) r.status = Status::DuplicateId;
    if (r.status != Status::Ok) return r;
    r.filled = match(id, side, true, price, qty, ts, fills);
    r.resting = qty - r.filled;
    if (r.resting > 0) book(side)[price].push_back(Order{id, r.resting, ts});
    return r;
  }

  SubmitResult submit_market(OrderId id, Side side, Qty qty, Time ts, std::vector<Fill>& fills) {
    SubmitResult r;
    if (id == kNoOrder) {
      r.status = Status::InvalidId;
    } else if (qty <= 0) {
      r.status = Status::InvalidQty;
    } else if (find(id)) {
      r.status = Status::DuplicateId;
    } else {
      r.filled = match(id, side, false, 0, qty, ts, fills);
    }
    return r;
  }

  SubmitResult replace(OrderId id, Price price, Qty qty, Time ts, std::vector<Fill>& fills) {
    SubmitResult r;
    r.status = validate(id, price, qty);
    if (r.status != Status::Ok) return r;
    const auto info = find(id);
    if (!info) {
      r.status = Status::UnknownId;
      return r;
    }
    if (price == info->price && qty <= info->qty) {
      locate(id)->qty = qty;
      r.resting = qty;
      return r;
    }
    cancel(id);
    return submit_limit(id, info->side, price, qty, ts, fills);
  }

  Status cancel(OrderId id) {
    if (id == kNoOrder) return Status::InvalidId;
    for (SideMap* m : {&bids_, &asks_}) {
      for (auto lv = m->begin(); lv != m->end(); ++lv) {
        for (auto it = lv->second.begin(); it != lv->second.end(); ++it) {
          if (it->id != id) continue;
          lv->second.erase(it);
          if (lv->second.empty()) m->erase(lv);
          return Status::Ok;
        }
      }
    }
    return Status::UnknownId;
  }

  Status reduce(OrderId id, Qty by) {
    if (id == kNoOrder) return Status::InvalidId;
    if (by <= 0) return Status::InvalidQty;
    Order* o = locate(id);
    if (!o) return Status::UnknownId;
    if (by >= o->qty) return cancel(id);
    o->qty -= by;
    return Status::Ok;
  }

  Status add_resting(OrderId id, Side side, Price price, Qty qty, Time ts) {
    Status s = validate(id, price, qty);
    if (s == Status::Ok && find(id)) s = Status::DuplicateId;
    if (s == Status::Ok) book(side)[price].push_back(Order{id, qty, ts});
    return s;
  }

  Status execute_resting(OrderId id, Qty qty, Time ts, std::vector<Fill>& fills) {
    if (id == kNoOrder) return Status::InvalidId;
    if (qty <= 0) return Status::InvalidQty;
    const auto info = find(id);
    if (!info) return Status::UnknownId;
    const Qty take = std::min(qty, info->qty);
    fills.push_back(Fill{id, kNoOrder, info->side, info->price, take, ts, info->qty - take});
    return reduce(id, take);
  }

  // All levels of a side, best first, written into `out` (cleared first; reused to save
  // allocations in the slow sanitizer build).
  void levels(Side side, std::vector<Level>& out) const {
    out.clear();
    auto add = [&](const std::pair<const Price, Queue>& lv) {
      Qty q = 0;
      for (const Order& o : lv.second) q += o.qty;
      out.push_back(Level{lv.first, q, static_cast<std::uint32_t>(lv.second.size())});
    };
    if (side == Side::Bid) {
      for (auto it = bids_.rbegin(); it != bids_.rend(); ++it) add(*it);
    } else {
      for (const auto& lv : asks_) add(lv);
    }
  }

  std::optional<OrderInfo> find(OrderId id) const {
    for (const Side side : {Side::Bid, Side::Ask}) {
      for (const auto& lv : book(side)) {
        for (const Order& o : lv.second) {
          if (o.id == id) return OrderInfo{id, side, lv.first, o.qty, o.ts};
        }
      }
    }
    return std::nullopt;
  }

  std::optional<Qty> queue_ahead(OrderId id) const {
    for (const SideMap* m : {&bids_, &asks_}) {
      for (const auto& lv : *m) {
        Qty ahead = 0;
        for (const Order& o : lv.second) {
          if (o.id == id) return ahead;
          ahead += o.qty;
        }
      }
    }
    return std::nullopt;
  }

  std::size_t order_count() const {
    std::size_t n = 0;
    for (const SideMap* m : {&bids_, &asks_}) {
      for (const auto& lv : *m) n += lv.second.size();
    }
    return n;
  }

  // Every resting order id (for choosing cancel targets in tests), written into `out`.
  void ids(std::vector<OrderId>& out) const {
    out.clear();
    for (const SideMap* m : {&bids_, &asks_}) {
      for (const auto& lv : *m) {
        for (const Order& o : lv.second) out.push_back(o.id);
      }
    }
  }

 private:
  static Status validate(OrderId id, Price price, Qty qty) {
    if (id == kNoOrder) return Status::InvalidId;
    if (price <= 0) return Status::InvalidPrice;
    if (qty <= 0) return Status::InvalidQty;
    return Status::Ok;
  }

  SideMap& book(Side s) { return s == Side::Bid ? bids_ : asks_; }
  const SideMap& book(Side s) const { return s == Side::Bid ? bids_ : asks_; }

  Order* locate(OrderId id) {
    for (SideMap* m : {&bids_, &asks_}) {
      for (auto& lv : *m) {
        for (Order& o : lv.second) {
          if (o.id == id) return &o;
        }
      }
    }
    return nullptr;
  }

  Qty match(OrderId aggressor, Side side, bool has_limit, Price limit, Qty qty, Time ts,
            std::vector<Fill>& fills) {
    SideMap& opp = book(opposite(side));
    Qty filled = 0;
    while (qty > 0 && !opp.empty()) {
      // Best opposite level: lowest ask, or highest bid.
      auto lv = side == Side::Bid ? opp.begin() : std::prev(opp.end());
      const Price p = lv->first;
      if (has_limit && (side == Side::Bid ? p > limit : p < limit)) break;
      Queue& q = lv->second;
      while (qty > 0 && !q.empty()) {
        Order& o = q.front();
        const Qty take = std::min(qty, o.qty);
        o.qty -= take;
        qty -= take;
        filled += take;
        fills.push_back(Fill{o.id, aggressor, opposite(side), p, take, ts, o.qty});
        if (o.qty == 0) q.pop_front();
      }
      if (q.empty()) opp.erase(lv);
    }
    return filled;
  }

  SideMap bids_;
  SideMap asks_;
};

// Compares every aggregate the engine exposes against the reference. Returns an empty string if
// they agree, otherwise a description of the first difference.
inline std::string diff_books(const OrderBook& book, const ReferenceBook& ref) {
  std::ostringstream out;
  if (book.order_count() != ref.order_count()) {
    out << "order_count " << book.order_count() << " vs " << ref.order_count();
    return out.str();
  }
  static std::vector<Level> got;  // reused across calls; the tests are single-threaded
  static std::vector<Level> want;
  for (const Side side : {Side::Bid, Side::Ask}) {
    const char* name = side == Side::Bid ? "bid" : "ask";
    ref.levels(side, want);
    book.depth(side, want.size() + 1, got);
    if (got.size() != want.size() || book.level_count(side) != want.size()) {
      out << name << " level count " << got.size() << "/" << book.level_count(side) << " vs "
          << want.size();
      return out.str();
    }
    Qty total = 0;
    for (std::size_t i = 0; i < want.size(); ++i) {
      total += want[i].qty;
      if (got[i].price != want[i].price || got[i].qty != want[i].qty ||
          got[i].count != want[i].count) {
        out << name << " level " << i << ": (" << got[i].price << ", " << got[i].qty << ", "
            << got[i].count << ") vs (" << want[i].price << ", " << want[i].qty << ", "
            << want[i].count << ")";
        return out.str();
      }
    }
    if (book.total_qty(side) != total) {
      out << name << " total_qty " << book.total_qty(side) << " vs " << total;
      return out.str();
    }
    const auto best = book.best(side);
    if (best.has_value() != !want.empty() ||
        (best && (best->price != want[0].price || best->qty != want[0].qty ||
                  best->count != want[0].count))) {
      out << name << " best() disagrees with depth()";
      return out.str();
    }
  }
  return {};
}

inline bool same_fill(const Fill& a, const Fill& b) {
  return a.resting_id == b.resting_id && a.aggressor_id == b.aggressor_id &&
         a.resting_side == b.resting_side && a.price == b.price && a.qty == b.qty &&
         a.ts == b.ts && a.resting_left == b.resting_left;
}

inline std::string describe(const Fill& f) {
  std::ostringstream out;
  out << "{rest " << f.resting_id << ", aggr " << f.aggressor_id << ", side "
      << static_cast<int>(f.resting_side) << ", px " << f.price << ", qty " << f.qty << ", ts "
      << f.ts << ", left " << f.resting_left << "}";
  return out.str();
}

// Empty if the two fill lists are identical, else the first difference.
inline std::string diff_fills(const std::vector<Fill>& got, const std::vector<Fill>& want) {
  const std::size_t n = std::min(got.size(), want.size());
  for (std::size_t i = 0; i < n; ++i) {
    if (!same_fill(got[i], want[i])) {
      return "fill " + std::to_string(i) + ": " + describe(got[i]) + " vs " + describe(want[i]);
    }
  }
  if (got.size() != want.size()) {
    return "fill count " + std::to_string(got.size()) + " vs " + std::to_string(want.size());
  }
  return {};
}

}  // namespace lob::testing
