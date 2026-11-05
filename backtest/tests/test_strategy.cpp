// Quoting rules: tick rounding, post-only clipping, inventory limit, and reduction of the A&S and
// symmetric strategies to the paper module's formulas.
#include <cmath>

#include "lob/avellaneda_stoikov.hpp"
#include "lob/backtest/strategy.hpp"
#include "lob/check.hpp"
#include "lob/rng.hpp"

using namespace lob;
using namespace lob::backtest;

namespace {

StrategyView view(Price bb, Price ba, Qty inv = 0, Qty qmax = 10) {
  StrategyView v;
  v.t = 0.0;
  v.horizon = 600.0;
  v.best_bid = bb;
  v.best_ask = ba;
  v.others_bid = bb;
  v.others_ask = ba;
  v.mid = 0.5 * static_cast<double>(bb + ba);
  v.mid_defined = true;
  v.inventory = inv;
  v.max_inventory = qmax;
  v.quote_size = 1;
  return v;
}

AsParams as_params(double gamma) {
  AsParams a;
  a.gamma = gamma;
  a.sigma = 0.7;
  a.k = 1.5;
  a.horizon = 600.0;
  a.requote = 0.1;
  return a;
}

}  // namespace

LOB_TEST(rounds_bid_down_and_ask_up) {
  const StrategyView v = view(9990, 10010);
  QuoteTarget q = apply_quote_rules(9995.7, 10004.2, true, true, v);
  LOB_CHECK(q.bid_on && q.ask_on);
  LOB_CHECK_EQ(q.bid, Price{9995});
  LOB_CHECK_EQ(q.ask, Price{10005});
  LOB_CHECK_EQ(q.lots, Qty{1});
  // Exact ticks stay where they are, also with floating-point noise just below/above.
  q = apply_quote_rules(9996.0, 10004.0, true, true, v);
  LOB_CHECK_EQ(q.bid, Price{9996});
  LOB_CHECK_EQ(q.ask, Price{10004});
  q = apply_quote_rules(9995.999999999998, 10004.000000000002, true, true, v);
  LOB_CHECK_EQ(q.bid, Price{9996});
  LOB_CHECK_EQ(q.ask, Price{10004});
  // Negative side of rounding: a raw bid just above an integer still floors to it.
  q = apply_quote_rules(9996.01, 10003.99, true, true, v);
  LOB_CHECK_EQ(q.bid, Price{9996});
  LOB_CHECK_EQ(q.ask, Price{10004});
}

LOB_TEST(post_only_clipping) {
  const StrategyView v = view(10000, 10001);
  // A bid at or through the ask is clipped to ask - 1; an ask at or through the bid to bid + 1.
  QuoteTarget q = apply_quote_rules(10003.2, 9998.1, true, true, v);
  LOB_CHECK_EQ(q.bid, Price{10000});
  LOB_CHECK_EQ(q.ask, Price{10001});
  q = apply_quote_rules(10001.0, 10000.0, true, true, v);
  LOB_CHECK_EQ(q.bid, Price{10000});
  LOB_CHECK_EQ(q.ask, Price{10001});
  // Randomized: the result never crosses the public book and never moves a quote towards the mid
  // except by clipping.
  Rng rng(7);
  for (int i = 0; i < 20000; ++i) {
    const Price bb = 10000 + static_cast<Price>(rng.uniform_int(std::int64_t{-5}, std::int64_t{5}));
    const Price ba = bb + static_cast<Price>(rng.uniform_int(std::int64_t{1}, std::int64_t{6}));
    const double rb = static_cast<double>(bb) + rng.uniform(-8.0, 8.0);
    const double ra = rb + rng.uniform(0.0, 10.0);
    const StrategyView w = view(bb, ba, 0, 10);
    const QuoteTarget t = apply_quote_rules(rb, ra, true, true, w);
    LOB_CHECK(t.bid_on && t.ask_on);
    LOB_CHECK(t.bid <= ba - 1);
    LOB_CHECK(t.ask >= bb + 1);
    LOB_CHECK(t.bid < t.ask);
    LOB_CHECK(static_cast<double>(t.bid) <= rb + 1e-9);
    LOB_CHECK(static_cast<double>(t.ask) >= ra - 1e-9);
    LOB_CHECK(t.bid == std::min<Price>(static_cast<Price>(std::floor(rb + 1e-9)), ba - 1));
    LOB_CHECK(t.ask == std::max<Price>(static_cast<Price>(std::ceil(ra - 1e-9)), bb + 1));
  }
}

LOB_TEST(inventory_limit_stops_the_side_that_adds) {
  for (Qty inv = -12; inv <= 12; ++inv) {
    const StrategyView v = view(9990, 10010, inv, 10);
    const QuoteTarget q = apply_quote_rules(9995.0, 10005.0, true, true, v);
    LOB_CHECK_EQ(q.bid_on, inv + 1 <= 10);
    LOB_CHECK_EQ(q.ask_on, inv - 1 >= -10);
  }
  // Q_max = 0: never quotes.
  const QuoteTarget z = apply_quote_rules(9995.0, 10005.0, true, true, view(9990, 10010, 0, 0));
  LOB_CHECK(!z.bid_on && !z.ask_on);
}

LOB_TEST(nonpositive_prices_and_unwanted_sides_are_not_quoted) {
  const StrategyView v = view(1, 5);
  const QuoteTarget q = apply_quote_rules(0.5, 3.0, true, true, v);
  LOB_CHECK(!q.bid_on);
  LOB_CHECK(q.ask_on);
  const QuoteTarget r = apply_quote_rules(2.0, 3.0, false, true, v);
  LOB_CHECK(!r.bid_on && r.ask_on);
  const QuoteTarget s = apply_quote_rules(std::nan(""), 3.0, true, true, v);
  LOB_CHECK(!s.bid_on);
}

LOB_TEST(as_raw_quotes_are_the_paper_formulas) {
  for (double gamma : {0.0005, 0.003, 0.02, 0.3}) {
    const AsParams a = as_params(gamma);
    const AvellanedaStoikov st(a);
    const paper::ModelParams mp = to_paper_params(a);
    LOB_CHECK_EQ(mp.steps, 6000);
    for (double t : {0.0, 100.0, 599.9}) {
      for (Qty q : {Qty{-7}, Qty{0}, Qty{3}}) {
        const double s = 10000.5;
        const paper::Quotes r = st.raw(t, s, q);
        const double res = paper::reservation_price(s, static_cast<double>(q), t, gamma, mp);
        const double spr = paper::optimal_spread(t, gamma, mp);
        LOB_CHECK_NEAR(r.bid, res - spr / 2, 1e-9);
        LOB_CHECK_NEAR(r.ask, res + spr / 2, 1e-9);
        // Written out: r = s - q gamma sigma^2 (T - t), spread = gamma sigma^2 (T - t) + (2/gamma)
        // ln(1 + gamma/k).
        const double r2 = s - static_cast<double>(q) * gamma * 0.49 * (600.0 - t);
        const double sp2 = gamma * 0.49 * (600.0 - t) + 2.0 / gamma * std::log(1.0 + gamma / 1.5);
        LOB_CHECK_NEAR(r.bid, r2 - sp2 / 2, 1e-7);
        LOB_CHECK_NEAR(r.ask, r2 + sp2 / 2, 1e-7);
      }
    }
  }
}

LOB_TEST(as_quote_applies_rules_to_paper_quotes) {
  AvellanedaStoikov st(as_params(0.003));
  Rng rng(11);
  for (int i = 0; i < 5000; ++i) {
    const Price bb = 10000 + static_cast<Price>(rng.uniform_int(std::int64_t{-3}, std::int64_t{3}));
    const Price ba = bb + static_cast<Price>(rng.uniform_int(std::int64_t{1}, std::int64_t{4}));
    StrategyView v = view(bb, ba, static_cast<Qty>(rng.uniform_int(std::int64_t{-10}, std::int64_t{10})));
    v.t = rng.uniform(0.0, 600.0);
    const QuoteTarget q = st.quote(v);
    const paper::Quotes r = st.raw(v.t, v.mid, v.inventory);
    const QuoteTarget e = apply_quote_rules(r.bid, r.ask, true, true, v);
    LOB_CHECK(q.bid_on == e.bid_on && q.ask_on == e.ask_on);
    LOB_CHECK(q.bid == e.bid && q.ask == e.ask);
  }
  // No defined mid: no quotes.
  StrategyView v = view(9999, 10001);
  v.mid_defined = false;
  const QuoteTarget q = st.quote(v);
  LOB_CHECK(!q.bid_on && !q.ask_on);
}

LOB_TEST(as_with_zero_inventory_is_symmetric_and_skews_with_inventory) {
  const AvellanedaStoikov st(as_params(0.01));
  const paper::Quotes q0 = st.raw(50.0, 10000.0, 0);
  LOB_CHECK_NEAR(0.5 * (q0.bid + q0.ask), 10000.0, 1e-9);
  const paper::Quotes qp = st.raw(50.0, 10000.0, 4);
  const paper::Quotes qn = st.raw(50.0, 10000.0, -4);
  LOB_CHECK(qp.bid < q0.bid && qp.ask < q0.ask);  // long: both quotes move down
  LOB_CHECK(qn.bid > q0.bid && qn.ask > q0.ask);  // short: both move up
  LOB_CHECK_NEAR(qp.ask - qp.bid, q0.ask - q0.bid, 1e-9);  // spread does not depend on q
  // At t = T the inventory term vanishes.
  const paper::Quotes qT = st.raw(600.0, 10000.0, 9);
  LOB_CHECK_NEAR(0.5 * (qT.bid + qT.ask), 10000.0, 1e-9);
}

LOB_TEST(symmetric_uses_the_paper_average_spread) {
  for (double gamma : {0.0003, 0.004, 0.05}) {
    const AsParams a = as_params(gamma);
    SymmetricMatched st(a);
    const double sbar = paper::average_optimal_spread(gamma, to_paper_params(a));
    LOB_CHECK_NEAR(st.spread(), sbar, 1e-12);
    StrategyView v = view(9990, 10010, 5);  // inventory has no effect
    v.t = 123.4;
    const QuoteTarget q = st.quote(v);
    const QuoteTarget e = apply_quote_rules(v.mid - sbar / 2, v.mid + sbar / 2, true, true, v);
    LOB_CHECK(q.bid == e.bid && q.ask == e.ask && q.bid_on && q.ask_on);
    v.inventory = -5;
    const QuoteTarget q2 = st.quote(v);
    LOB_CHECK(q2.bid == q.bid && q2.ask == q.ask);
  }
}

LOB_TEST(naive_joins_the_others_touch) {
  NaiveTouch st;
  StrategyView v = view(9998, 10003);
  // Our own bid improved the public best bid; naive joins the others' best, not our own.
  v.best_bid = 10000;
  QuoteTarget q = st.quote(v);
  LOB_CHECK(q.bid_on && q.ask_on);
  LOB_CHECK_EQ(q.bid, Price{9998});
  LOB_CHECK_EQ(q.ask, Price{10003});
  v.others_ask.reset();
  q = st.quote(v);
  LOB_CHECK(q.bid_on && !q.ask_on);
  v = view(9998, 10003, 10);  // at the limit: no bid
  q = st.quote(v);
  LOB_CHECK(!q.bid_on && q.ask_on);
}

LOB_TEST(fixed_offset_quotes_mid_plus_minus_offset) {
  FixedOffset st(2.5);
  const StrategyView v = view(9999, 10002);  // mid 10000.5
  const QuoteTarget q = st.quote(v);
  LOB_CHECK_EQ(q.bid, Price{9998});
  LOB_CHECK_EQ(q.ask, Price{10003});
}

LOB_TEST_MAIN()
