#include "lob/backtest/params.hpp"

#include <cmath>
#include <cstdio>

namespace lob::backtest {

std::vector<ParamField> param_fields(SimParams& p) {
  auto d = [](const char* name, const char* unit, const char* group, const char* why, double& v) {
    ParamField f{name, unit, group, why};
    f.d = &v;
    return f;
  };
  auto i = [](const char* name, const char* unit, const char* group, const char* why,
              std::int64_t& v) {
    ParamField f{name, unit, group, why};
    f.i = &v;
    return f;
  };
  return {
      d("horizon", "s", "timing", "A&S horizon T; 10 minutes is long enough for inventory to matter",
        p.horizon),
      d("burn-in", "s", "timing", "100 mean order lifetimes, so the initial book is forgotten",
        p.burn_in),
      d("markout-tail", "s", "timing", "longest markout horizon, so fills near T get markouts",
        p.markout_tail),
      i("ref-price", "ticks", "background", "100 USD at 0.01 USD per tick", p.ref_price),
      d("lo-rate", "1/s per side", "background",
        "limit-order rate at distance 1; with theta gives a few lots at the touch", p.lo_rate),
      d("lo-alpha", "-", "background",
        "power-law decay of the rate with distance, the functional form of CST (2010)", p.lo_alpha),
      i("lo-levels", "ticks", "background", "orders land within 10 ticks of the opposite best",
        p.lo_levels),
      d("cancel-hazard", "1/s", "background",
        "mean lifetime 3.3 s: the book refills within seconds after a sweep", p.cancel_hazard),
      d("lo-size-mean", "lots", "background", "1 + Geometric sizes, mean 2 lots", p.lo_size_mean),
      i("init-levels", "levels", "background", "initial book only; replaced during burn-in",
        p.init_levels),
      i("init-lots", "lots", "background", "initial book only; replaced during burn-in",
        p.init_lots),
      d("mo-rate", "1/s", "background",
        "one market order per second; with the depth gives a spread of 1-2 ticks", p.mo_rate),
      d("mo-size-mean", "lots", "background",
        "same size law as limit orders, so most market orders stay at the touch", p.mo_size_mean),
      d("news-rate", "1/s", "informed", "nu: 0 = no informed flow; the stress grid varies it",
        p.news_rate),
      d("branching", "-", "informed",
        "n_br: mean cluster size 1/(1 - n_br); the stress grid varies it", p.branching),
      d("hawkes-decay", "1/s", "informed",
        "beta: children follow their parent after 0.5 s on average (seconds-scale bursts)",
        p.hawkes_decay),
      d("inf-size-mean", "lots", "informed", "same size law as background market orders",
        p.inf_size_mean),
      d("latency", "s", "trading", "1 ms decision-to-book delay (simplified, fixed)", p.latency),
      d("requote", "s", "trading", "decision every 100 ms", p.requote),
      i("max-inventory", "lots", "trading", "Q_max: hard position limit", p.max_inventory),
      i("quote-size", "lots", "trading", "one lot per side", p.quote_size),
      i("check-every", "events", "checks", "engine invariant checks (tests); 0 = off",
        p.check_every),
  };
}

std::string validate(const SimParams& p) {
  char buf[160];
  auto bad = [&](const char* what) {
    std::snprintf(buf, sizeof buf, "invalid parameter: %s", what);
    return std::string(buf);
  };
  if (!(p.horizon > 0.0) || !(p.horizon <= 86400.0)) return bad("horizon must be in (0, 86400]");
  if (!(p.burn_in >= 0.0) || !(p.markout_tail >= 0.0)) return bad("burn-in and tail must be >= 0");
  if (p.ref_price < 1000) return bad("ref-price must be >= 1000 ticks");
  if (!(p.lo_rate > 0.0) || !(p.lo_alpha >= 0.0)) return bad("lo-rate > 0 and lo-alpha >= 0");
  if (p.lo_levels < 1 || p.lo_levels > 1000) return bad("lo-levels in [1, 1000]");
  if (!(p.cancel_hazard > 0.0)) return bad("cancel-hazard must be > 0");
  if (!(p.lo_size_mean >= 1.0) || !(p.mo_size_mean >= 1.0) || !(p.inf_size_mean >= 1.0))
    return bad("size means must be >= 1 lot");
  if (p.init_levels < 1 || p.init_lots < 1) return bad("init-levels and init-lots >= 1");
  if (!(p.mo_rate >= 0.0)) return bad("mo-rate must be >= 0");
  if (!(p.news_rate >= 0.0)) return bad("news-rate must be >= 0");
  if (!(p.branching >= 0.0) || !(p.branching < 1.0)) return bad("branching must be in [0, 1)");
  if (!(p.hawkes_decay > 0.0)) return bad("hawkes-decay must be > 0");
  if (!(p.latency >= 0.0)) return bad("latency must be >= 0");
  if (!(p.requote > 0.0)) return bad("requote must be > 0");
  if (p.max_inventory < 0) return bad("max-inventory must be >= 0");
  if (p.quote_size < 1) return bad("quote-size must be >= 1");
  if (p.check_every < 0) return bad("check-every must be >= 0");
  return {};
}

Time seconds_to_ns(double s) noexcept {
  return static_cast<Time>(std::llround(s * static_cast<double>(kNanosPerSecond)));
}

}  // namespace lob::backtest
