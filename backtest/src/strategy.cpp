#include "lob/backtest/strategy.hpp"

#include <algorithm>
#include <cmath>

namespace lob::backtest {

namespace {
// Guards against values like 9999.999999999998 that are meant to be an exact tick.
constexpr double kRoundEps = 1e-9;
}  // namespace

QuoteTarget apply_quote_rules(double raw_bid, double raw_ask, bool want_bid, bool want_ask,
                              const StrategyView& v) {
  QuoteTarget q;
  q.lots = v.quote_size;
  if (want_bid && std::isfinite(raw_bid)) {
    Price b = static_cast<Price>(std::floor(raw_bid + kRoundEps));
    if (v.best_ask) b = std::min(b, *v.best_ask - 1);
    const bool limit_ok = v.inventory + v.quote_size <= v.max_inventory;
    if (b >= 1 && limit_ok) {
      q.bid_on = true;
      q.bid = b;
    }
  }
  if (want_ask && std::isfinite(raw_ask)) {
    Price a = static_cast<Price>(std::ceil(raw_ask - kRoundEps));
    if (v.best_bid) a = std::max(a, *v.best_bid + 1);
    const bool limit_ok = v.inventory - v.quote_size >= -v.max_inventory;
    if (a >= 1 && limit_ok) {
      q.ask_on = true;
      q.ask = a;
    }
  }
  return q;
}

paper::ModelParams to_paper_params(const AsParams& a) {
  paper::ModelParams m;
  m.s0 = 0.0;  // unused by the quoting formulas
  m.sigma = a.sigma;
  m.horizon = a.horizon;
  m.k = a.k;
  m.A = 0.0;   // unused by the quoting formulas
  m.steps = std::max(1, static_cast<int>(std::lround(a.horizon / a.requote)));
  return m;
}

AvellanedaStoikov::AvellanedaStoikov(const AsParams& a)
    : a_(a), rule_(paper::Strategy::Inventory, a.gamma, to_paper_params(a)) {}

paper::Quotes AvellanedaStoikov::raw(double t, double s, Qty q) const {
  return rule_(0, t, s, q);
}

QuoteTarget AvellanedaStoikov::quote(const StrategyView& v) {
  if (!v.mid_defined) return {};
  const paper::Quotes r = raw(std::min(v.t, a_.horizon), v.mid, v.inventory);
  return apply_quote_rules(r.bid, r.ask, true, true, v);
}

SymmetricMatched::SymmetricMatched(const AsParams& a)
    : rule_(paper::Strategy::Symmetric, a.gamma, to_paper_params(a)) {}

QuoteTarget SymmetricMatched::quote(const StrategyView& v) {
  if (!v.mid_defined) return {};
  const paper::Quotes r = rule_(0, v.t, v.mid, v.inventory);
  return apply_quote_rules(r.bid, r.ask, true, true, v);
}

SymmetricTimeVarying::SymmetricTimeVarying(const AsParams& a)
    : a_(a), rule_(paper::Strategy::SymmetricTimeMatched, a.gamma, to_paper_params(a)) {}

paper::Quotes SymmetricTimeVarying::raw(double t, double s) const {
  return rule_(0, t, s, 0);
}

QuoteTarget SymmetricTimeVarying::quote(const StrategyView& v) {
  if (!v.mid_defined) return {};
  const paper::Quotes r = raw(std::min(v.t, a_.horizon), v.mid);
  return apply_quote_rules(r.bid, r.ask, true, true, v);
}

SkewConstSpread::SkewConstSpread(const AsParams& a)
    : a_(a),
      m_(to_paper_params(a)),
      half_spread_(0.5 * paper::average_optimal_spread(a.gamma, m_)) {}

paper::Quotes SkewConstSpread::raw(double t, double s, Qty q) const {
  const double r = paper::reservation_price(s, static_cast<double>(q), t, a_.gamma, m_);
  return {r - half_spread_, r + half_spread_};
}

QuoteTarget SkewConstSpread::quote(const StrategyView& v) {
  if (!v.mid_defined) return {};
  const paper::Quotes r = raw(std::min(v.t, a_.horizon), v.mid, v.inventory);
  return apply_quote_rules(r.bid, r.ask, true, true, v);
}

QuoteTarget NaiveTouch::quote(const StrategyView& v) {
  const double b = v.others_bid ? static_cast<double>(*v.others_bid) : 0.0;
  const double a = v.others_ask ? static_cast<double>(*v.others_ask) : 0.0;
  return apply_quote_rules(b, a, v.others_bid.has_value(), v.others_ask.has_value(), v);
}

QuoteTarget FixedOffset::quote(const StrategyView& v) {
  if (!v.mid_defined) return {};
  return apply_quote_rules(v.mid - offset_, v.mid + offset_, true, true, v);
}

}  // namespace lob::backtest
