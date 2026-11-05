#include "lob/lobster_stats.hpp"

namespace lob::lobster {

Stats::Top Stats::top_of(const OrderBook& book) noexcept {
  Top t;
  if (const auto b = book.best_bid()) {
    t.bid = true;
    t.bid_px = b->price;
    t.bid_qty = b->qty;
  }
  if (const auto a = book.best_ask()) {
    t.ask = true;
    t.ask_px = a->price;
    t.ask_qty = a->qty;
  }
  return t;
}

void Stats::start(const OrderBook& book) noexcept { top_ = top_of(book); }

void Stats::observe(const Message& m, const OrderBook& book) noexcept {
  ++r_.messages;
  if (m.type >= 1 && m.type <= 7) ++r_.by_type[static_cast<std::size_t>(m.type)];
  if (m.type == 4) {
    (m.direction > 0 ? r_.executed_bid : r_.executed_ask) += m.size;
  } else if (m.type == 5) {
    if (m.direction == 1) {
      r_.hidden_buy += m.size;
    } else if (m.direction == -1) {
      r_.hidden_sell += m.size;
    } else {
      r_.hidden_other += m.size;
    }
  }

  // Weight the state before this message by the time since the previous message.
  if (have_time_ && m.time > last_time_) {
    const auto dt = static_cast<double>(m.time - last_time_);
    if (top_.bid && top_.ask) {
      spread_ns_ += static_cast<double>(top_.ask_px - top_.bid_px) * dt;
      two_sided_ns_ += dt;
    }
    if (top_.bid) {
      bid_qty_ns_ += static_cast<double>(top_.bid_qty) * dt;
      bid_ns_ += dt;
    }
    if (top_.ask) {
      ask_qty_ns_ += static_cast<double>(top_.ask_qty) * dt;
      ask_ns_ += dt;
    }
  }
  if (!have_time_ || m.time >= last_time_) last_time_ = m.time;
  have_time_ = true;

  const Top now = top_of(book);
  const bool price_changed = now.bid != top_.bid || now.ask != top_.ask ||
                             (now.bid && now.bid_px != top_.bid_px) ||
                             (now.ask && now.ask_px != top_.ask_px);
  const bool quote_changed = price_changed || (now.bid && now.bid_qty != top_.bid_qty) ||
                             (now.ask && now.ask_qty != top_.ask_qty);
  if (price_changed) ++r_.best_price_changes;
  if (quote_changed) ++r_.best_quote_changes;
  top_ = now;
}

StatsResult Stats::result() const noexcept {
  StatsResult r = r_;
  r.two_sided_seconds = two_sided_ns_ / 1e9;
  r.mean_spread_ticks = two_sided_ns_ > 0 ? spread_ns_ / two_sided_ns_ : 0.0;
  r.mean_best_bid_qty = bid_ns_ > 0 ? bid_qty_ns_ / bid_ns_ : 0.0;
  r.mean_best_ask_qty = ask_ns_ > 0 ? ask_qty_ns_ / ask_ns_ : 0.0;
  return r;
}

}  // namespace lob::lobster
