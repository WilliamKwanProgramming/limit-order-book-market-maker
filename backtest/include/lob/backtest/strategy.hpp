// Quoting strategies for the backtest simulator.
//
// A strategy is asked for its quotes every `requote` seconds. It sees only what a market maker
// would see at that moment (StrategyView): the clock, the public book, the book without its own
// orders, its own working orders, its inventory and its fills (on_fill). It returns a QuoteTarget;
// the simulator's order manager keeps an order whose price and size are unchanged (queue position
// preserved) and otherwise cancels it and submits a new one, both after the configured latency.
//
// Common quoting rules (apply_quote_rules), used by every strategy here:
//   - prices are rounded onto the tick grid away from the mid: bid floored, ask ceiled;
//   - post-only: bid <= best ask - 1 and ask >= best bid + 1 (public book, so never self-cross);
//   - inventory limit: no bid if inventory + size > Q_max, no ask if inventory - size < -Q_max;
//   - prices below 1 tick are not quoted.
// The simulator enforces post-only and the inventory limit again when an order reaches the book
// (the market may have moved during the latency); see market.hpp.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "lob/avellaneda_stoikov.hpp"
#include "lob/order_book.hpp"
#include "lob/rng.hpp"
#include "lob/types.hpp"

namespace lob::backtest {

// One fill of our order (one engine Fill record).
struct OwnFill {
  Time t = 0;                // ns, simulator clock
  Side side = Side::Bid;     // side of OUR order: Bid = we bought
  Price price = 0;           // ticks
  Qty lots = 0;
  double mid_before = 0.0;   // others' mid (ticks) just before the aggressing market order
};

// Our working order on one side, as the order manager knows it.
struct OwnOrderView {
  bool active = false;  // submitted and not known to be filled, cancelled or rejected
  bool live = false;    // resting in the book (false while in flight)
  Price price = 0;
  Qty lots = 0;         // remaining
};

struct StrategyView {
  double t = 0.0;            // seconds since the episode started
  double horizon = 0.0;      // T, seconds
  // Public book (includes our own orders).
  std::optional<Price> best_bid;
  std::optional<Price> best_ask;
  // Book without our own orders ("others"). mid is the others' mid, or the last defined one if a
  // side is empty (mid_defined = false then).
  std::optional<Price> others_bid;
  std::optional<Price> others_ask;
  double mid = 0.0;
  bool mid_defined = false;
  Qty inventory = 0;         // lots
  Qty max_inventory = 0;     // Q_max, lots
  Qty quote_size = 1;        // lots
  OwnOrderView own_bid;
  OwnOrderView own_ask;
  const OrderBook* book = nullptr;  // read-only access to depth()
  Rng* rng = nullptr;               // the strategy's own random stream (no strategy here uses it)
};

struct QuoteTarget {
  bool bid_on = false;
  Price bid = 0;
  bool ask_on = false;
  Price ask = 0;
  Qty lots = 0;
};

// Applies the common rules above to unrounded prices (ticks). want_bid / want_ask = false leaves
// that side unquoted.
QuoteTarget apply_quote_rules(double raw_bid, double raw_ask, bool want_bid, bool want_ask,
                              const StrategyView& v);

class Strategy {
 public:
  virtual ~Strategy() = default;
  virtual QuoteTarget quote(const StrategyView& v) = 0;
  virtual void on_fill(const OwnFill& /*fill*/) {}
};

// Never quotes.
class NeverQuote final : public Strategy {
 public:
  QuoteTarget quote(const StrategyView&) override { return {}; }
};

// Parameters the A&S formulas need, in simulator units: sigma in ticks per sqrt(second), k per
// tick, gamma per (tick * lot), horizon T and requote interval in seconds.
struct AsParams {
  double gamma = 0.01;
  double sigma = 1.0;
  double k = 1.0;
  double horizon = 600.0;
  double requote = 0.1;
};

// The paper module's ModelParams for these units: sigma, k, horizon T, steps = round(T / requote)
// (so the paper's grid average spread is the average over our quoting times).
paper::ModelParams to_paper_params(const AsParams& a);

// Avellaneda-Stoikov inventory strategy: quotes r -/+ spread(t)/2 with
// r = s - q gamma sigma^2 (T - t) and spread(t) = gamma sigma^2 (T - t) + (2/gamma) ln(1 + gamma/k)
// (lob::paper::QuotingRule, Strategy::Inventory), s = others' mid, q = inventory in lots, then the
// common rules.
class AvellanedaStoikov final : public Strategy {
 public:
  explicit AvellanedaStoikov(const AsParams& a);
  QuoteTarget quote(const StrategyView& v) override;
  // Unrounded quotes before the common rules (exactly the paper's formulas).
  paper::Quotes raw(double t, double s, Qty q) const;

 private:
  AsParams a_;
  paper::QuotingRule rule_;
};

// The paper's benchmark: mid -/+ s_bar / 2 with s_bar the A&S average spread over the quoting
// times (lob::paper::QuotingRule, Strategy::Symmetric). No inventory term.
class SymmetricMatched final : public Strategy {
 public:
  explicit SymmetricMatched(const AsParams& a);
  QuoteTarget quote(const StrategyView& v) override;
  double spread() const noexcept { return rule_.symmetric_spread(); }

 private:
  paper::QuotingRule rule_;
};

// Symmetric quoting with the A&S time-varying spread: mid -/+ spread(t)/2 with
// spread(t) = gamma sigma^2 (T - t) + (2/gamma) ln(1 + gamma/k) at the current time (capped at T,
// as for A&S), no inventory term (lob::paper::QuotingRule, Strategy::SymmetricTimeMatched).
// It differs from AvellanedaStoikov only by the skew and from SymmetricMatched only by the time
// variation of the spread, so comparing the three separates the two effects.
class SymmetricTimeVarying final : public Strategy {
 public:
  explicit SymmetricTimeVarying(const AsParams& a);
  QuoteTarget quote(const StrategyView& v) override;
  // Unrounded quotes before the common rules.
  paper::Quotes raw(double t, double s) const;

 private:
  AsParams a_;
  paper::QuotingRule rule_;
};

// Joins the others' best bid and best ask (no inventory term). A side whose others' book is empty
// is not quoted.
class NaiveTouch final : public Strategy {
 public:
  QuoteTarget quote(const StrategyView& v) override;
};

// Probe: quotes mid -/+ offset ticks (then the common rules). Used to measure fill rate against
// quote distance.
class FixedOffset final : public Strategy {
 public:
  explicit FixedOffset(double offset) : offset_(offset) {}
  QuoteTarget quote(const StrategyView& v) override;

 private:
  double offset_;
};

}  // namespace lob::backtest
