#include "lob/lobster_replayer.hpp"

#include <algorithm>

namespace lob::lobster {

const char* to_string(Policy p) noexcept { return p == Policy::Strict ? "strict" : "lenient"; }

const char* to_string(Anomaly a) noexcept {
  switch (a) {
    case Anomaly::UnknownId: return "unknown_id";
    case Anomaly::DuplicateId: return "duplicate_id";
    case Anomaly::SizeExceedsRemaining: return "size_exceeds_remaining";
    case Anomaly::PartialCancelRemovesOrder: return "partial_cancel_removes_order";
    case Anomaly::SideMismatch: return "side_mismatch";
    case Anomaly::PriceMismatch: return "price_mismatch";
    case Anomaly::DeleteSizeMismatch: return "delete_size_mismatch";
    case Anomaly::TimeBackwards: return "time_backwards";
  }
  return "unknown";
}

Replayer::Replayer(Config cfg) : cfg_(cfg) {
  ghosts_.enabled = cfg.bootstrap;
  book_.reserve(std::size_t{1} << 16, std::size_t{1} << 10);
  fills_.reserve(8);
}

void Replayer::record(Anomaly a, AnomalyMask& mask) noexcept {
  mask |= bit(a);
  ++counters_.anomalies[static_cast<std::size_t>(a)];
}

namespace {

// Undo message `m` on one side of an order-book row (non-empty levels, best first, at most n).
// Returns false if the row is inconsistent with the message.
bool undo_on_side(const Message& m, std::vector<BookLevel>& lv, std::size_t n) {
  const bool bid = m.side() == Side::Bid;
  const auto better = [bid](Price a, Price b) { return bid ? a > b : a < b; };
  auto it =
      std::find_if(lv.begin(), lv.end(), [&](const BookLevel& l) { return l.price == m.price; });
  if (m.type == 1) {
    if (it == lv.end()) {
      // Not visible: fine only if the side is full and the price lies behind its last level.
      return lv.size() == n && better(lv.back().price, m.price);
    }
    if (it->qty < m.size) return false;
    it->qty -= m.size;
    if (it->qty == 0) lv.erase(it);
    return true;
  }
  // Types 2, 3, 4 removed size at this price: put it back.
  if (it != lv.end()) {
    it->qty += m.size;
    return true;
  }
  const auto pos = std::find_if(lv.begin(), lv.end(),
                                [&](const BookLevel& l) { return better(m.price, l.price); });
  lv.insert(pos, BookLevel{m.price, m.size});
  if (lv.size() > n) lv.pop_back();
  return true;
}

}  // namespace

bool Replayer::seed_ghosts(const Message& first, const BookRow& first_row) {
  if (!cfg_.bootstrap || ghosts_.seeded || counters_.messages > 0) return false;
  const std::size_t n = first_row.levels();
  std::vector<BookLevel> asks;
  std::vector<BookLevel> bids;
  for (std::size_t i = 0; i < n; ++i) {
    if (!first_row.asks[i].empty()) asks.push_back(first_row.asks[i]);
    if (!first_row.bids[i].empty()) bids.push_back(first_row.bids[i]);
  }
  if (changes_visible_book(first.type)) {
    ghosts_.first_message_undone =
        undo_on_side(first, first.side() == Side::Bid ? bids : asks, n);
  }
  OrderId next = 0;
  const auto seed = [&](const std::vector<BookLevel>& lv, Side side) {
    for (const BookLevel& l : lv) {
      const OrderId id = kGhostIdBit | ++next;
      if (book_.add_resting(id, side, l.price, l.qty, first.time) != Status::Ok) {
        ++counters_.engine_rejections;
        continue;
      }
      ghost_list_.push_back(Ghost{id, side, l.price});
      ++ghosts_.created;
      ghosts_.created_qty += l.qty;
    }
  };
  seed(bids, Side::Bid);
  seed(asks, Side::Ask);
  ghosts_.seeded = true;
  return true;
}

AnomalyMask Replayer::apply_to_ghosts(const Message& m, bool& resolved) {
  // Size still held by ghost orders at this side and price.
  Qty available = 0;
  for (const Ghost& g : ghost_list_) {
    if (g.side != m.side() || g.price != m.price) continue;
    if (const auto info = book_.find(g.id)) available += info->qty;
  }
  resolved = available > 0;
  AnomalyMask mask = 0;
  if (!resolved) {
    ++ghosts_.unexplained;
    record(Anomaly::UnknownId, mask);
    return mask;
  }
  if (m.size > available) {
    ++ghosts_.shortfall;
    record(Anomaly::SizeExceedsRemaining, mask);
  }
  return mask;
}

AnomalyMask Replayer::apply(const Message& m, std::uint64_t index, std::uint64_t line) {
  if (stopped_) return 0;
  ++counters_.messages;
  if (m.type >= 1 && m.type <= 7) ++counters_.by_type[static_cast<std::size_t>(m.type)];

  AnomalyMask mask = 0;
  if (have_time_ && m.time < last_time_) record(Anomaly::TimeBackwards, mask);
  have_time_ = true;
  last_time_ = m.time;

  const OrderId id = m.order_id();
  bool call_engine = changes_visible_book(m.type);
  bool use_ghosts = false;
  if (m.type == 1) {
    if (book_.find(id)) {
      record(Anomaly::DuplicateId, mask);
      call_engine = false;
    }
  } else if (changes_visible_book(m.type)) {
    const auto info = book_.find(id);
    if (!info) {
      call_engine = false;
      if (cfg_.bootstrap && ghosts_.seeded) {
        bool resolved = false;
        mask |= apply_to_ghosts(m, resolved);
        use_ghosts = resolved;
      } else {
        record(Anomaly::UnknownId, mask);
      }
    } else {
      if (info->side != m.side()) record(Anomaly::SideMismatch, mask);
      if (info->price != m.price) record(Anomaly::PriceMismatch, mask);
      if (m.type == 2) {
        if (m.size > info->qty) {
          record(Anomaly::SizeExceedsRemaining, mask);
        } else if (m.size == info->qty) {
          record(Anomaly::PartialCancelRemovesOrder, mask);
        }
      } else if (m.type == 3) {
        if (m.size != info->qty) record(Anomaly::DeleteSizeMismatch, mask);
      } else if (m.size > info->qty) {
        record(Anomaly::SizeExceedsRemaining, mask);
      }
    }
  }

  if (mask != 0) {
    ++counters_.messages_with_anomaly;
    if (!first_.set) first_ = FirstAnomaly{true, index, line, m, mask};
    if (cfg_.policy == Policy::Strict) {
      stopped_ = true;
      return mask;
    }
  }

  if (use_ghosts) {
    Qty need = m.size;
    for (const Ghost& g : ghost_list_) {
      if (need == 0) break;
      if (g.side != m.side() || g.price != m.price) continue;
      const auto info = book_.find(g.id);
      if (!info) continue;
      const Qty take = std::min(need, info->qty);
      Status s;
      if (m.type == 4) {
        fills_.clear();
        s = book_.execute_resting(g.id, take, m.time, fills_);
      } else {
        s = book_.reduce(g.id, take);
      }
      if (s != Status::Ok) {
        ++counters_.engine_rejections;
        continue;
      }
      need -= take;
      ghosts_.consumed_qty += take;
      if (take == info->qty) ++ghosts_.emptied;
    }
    ++ghosts_.references_resolved;
    return mask;
  }

  if (!call_engine) {
    if (changes_visible_book(m.type)) ++counters_.skipped_engine_calls;
    return mask;
  }
  Status s = Status::Ok;
  switch (m.type) {
    case 1: s = book_.add_resting(id, m.side(), m.price, m.size, m.time); break;
    case 2: s = book_.reduce(id, m.size); break;
    case 3: s = book_.cancel(id); break;
    case 4:
      fills_.clear();
      s = book_.execute_resting(id, m.size, m.time, fills_);
      break;
    default: break;
  }
  if (s != Status::Ok) ++counters_.engine_rejections;
  return mask;
}

}  // namespace lob::lobster
