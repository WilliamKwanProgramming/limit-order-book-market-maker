#include "lob/backtest/market.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace lob::backtest {

namespace {

// Random stream ids (Rng::for_stream(episode_seed, id)). One per exogenous ingredient.
enum : std::uint64_t {
  kStreamLimitBid = 1,     // background bid arrivals: interarrival times and distances
  kStreamLimitAsk = 2,     // background ask arrivals
  kStreamLimitSize = 3,    // background limit order sizes
  kStreamLifetime = 4,     // seed of the per-order lifetime streams
  kStreamMarket = 5,       // background market orders: interarrival times and sides
  kStreamMarketSize = 6,   // background market order sizes
  kStreamInformed = 7,     // Hawkes cluster times and directions
  kStreamInformedSize = 8, // informed market order sizes
  kStreamStrategy = 9,     // the strategy's own stream
};

// Event classes, in processing order at equal times.
enum : std::uint8_t {
  kClsLimitBid = 0,
  kClsLimitAsk = 1,
  kClsMarket = 2,
  kClsInformed = 3,
  kClsCancel = 4,
  kClsAction = 5,
  kClsRequote = 6,
  kClsEnd = 7,
};

enum : std::uint8_t { kActCancel = 0, kActSubmit = 1 };

std::size_t side_ix(Side s) { return s == Side::Bid ? 0 : 1; }

}  // namespace

std::uint64_t episode_seed(std::uint64_t seed, std::uint64_t e) {
  return Rng::for_stream(seed, e).next_u64();
}

Market::Market(const SimParams& p, std::uint64_t seed)
    : p_(p),
      rng_lo_bid_(Rng::for_stream(seed, kStreamLimitBid)),
      rng_lo_ask_(Rng::for_stream(seed, kStreamLimitAsk)),
      rng_lo_size_(Rng::for_stream(seed, kStreamLimitSize)),
      rng_mo_(Rng::for_stream(seed, kStreamMarket)),
      rng_mo_size_(Rng::for_stream(seed, kStreamMarketSize)),
      rng_inf_size_(Rng::for_stream(seed, kStreamInformedSize)),
      rng_strategy_(Rng::for_stream(seed, kStreamStrategy)),
      lifetime_seed_(Rng::for_stream(seed, kStreamLifetime).next_u64()) {
  t_start_ = seconds_to_ns(p_.burn_in);
  t_end_ = t_start_ + seconds_to_ns(p_.horizon);
  t_tail_ = t_end_ + seconds_to_ns(p_.markout_tail);

  // Distance distribution: P(i) proportional to i^-alpha, i = 1..K.
  double total = 0.0;
  for (std::int64_t i = 1; i <= p_.lo_levels; ++i) {
    total += std::pow(static_cast<double>(i), -p_.lo_alpha);
    distance_cdf_.push_back(total);
  }
  for (double& c : distance_cdf_) c /= total;
  distance_cdf_.back() = 1.0;
  lo_total_rate_ = p_.lo_rate * total;

  Rng rng_inf = Rng::for_stream(seed, kStreamInformed);
  generate_hawkes(rng_inf, {p_.news_rate, p_.branching, p_.hawkes_decay}, 0.0,
                  p_.burn_in + p_.horizon + p_.markout_tail, informed_);

  book_.reserve(4096, 1024);
  fills_.reserve(256);
  heap_.reserve(4096);

  // Initial book at t = 0 (part of the burn-in).
  for (std::int64_t lvl = 1; lvl <= p_.init_levels; ++lvl) {
    for (Side s : {Side::Bid, Side::Ask}) {
      const OrderId id = ++bg_count_;
      const Price price = p_.ref_price - sign(s) * lvl;
      book_.submit_limit(id, s, price, p_.init_lots * kSharesPerLot, 0, fills_);
      const double life = Rng::for_stream(lifetime_seed_, id).exponential(p_.cancel_hazard);
      Event c;
      c.t = seconds_to_ns(life);
      c.cls = kClsCancel;
      c.sub = id;
      c.id = id;
      push(c);
    }
  }
  fills_.clear();

  Event e;
  e.cls = kClsLimitBid;
  e.t = seconds_to_ns(rng_lo_bid_.exponential(lo_total_rate_));
  push(e);
  e.cls = kClsLimitAsk;
  e.t = seconds_to_ns(rng_lo_ask_.exponential(lo_total_rate_));
  push(e);
  if (p_.mo_rate > 0.0) {
    e.cls = kClsMarket;
    e.t = seconds_to_ns(rng_mo_.exponential(p_.mo_rate));
    push(e);
  }
  if (!informed_.empty()) {
    e.cls = kClsInformed;
    e.sub = 0;
    e.t = seconds_to_ns(informed_[0].t);
    push(e);
  }
  refresh_mid();
}

void Market::push(const Event& e) {
  heap_.push_back(e);
  std::push_heap(heap_.begin(), heap_.end(), Later{});
}

Market::Event Market::pop() {
  std::pop_heap(heap_.begin(), heap_.end(), Later{});
  const Event e = heap_.back();
  heap_.pop_back();
  return e;
}

Qty Market::draw_size(Rng& r, double mean) const {
  if (mean <= 1.0) return 1;
  // 1 + Geometric on {0, 1, ...} with P(X >= j) = q^j, q = 1 - 1/mean.
  const double q = 1.0 - 1.0 / mean;
  const double x = std::floor(std::log(r.uniform_open()) / std::log(q));
  return 1 + static_cast<Qty>(std::min(x, 1000.0));
}

void Market::burn_in() {
  while (!heap_.empty() && heap_.front().t < t_start_) {
    const Event e = pop();
    advance_to(e.t);
    process(e, nullptr);
  }
}

EpisodeResult Market::run_episode(Strategy& strategy, EpisodeLog* log) {
  if (consumed_) return res_;
  consumed_ = true;
  strategy_ = &strategy;
  log_ = log;
  trading_ = true;
  res_ = EpisodeResult{};
  own_fills_.clear();
  mid_path_.clear();
  mid_path_.push_back({t_start_, mid_});
  const auto nbins = static_cast<std::size_t>(std::ceil(p_.horizon));
  mo_bins_.assign(nbins, 0);
  std::vector<std::int64_t> fill_bins(nbins, 0);

  Event w;
  w.t = t_start_;
  w.cls = kClsRequote;
  push(w);
  Event end;
  end.t = t_end_;
  end.cls = kClsEnd;
  push(end);

  std::size_t counted_fills = 0;
  while (!heap_.empty() && heap_.front().t <= t_tail_) {
    const Event e = pop();
    advance_to(e.t);
    process(e, &strategy);
    for (; counted_fills < own_fills_.size(); ++counted_fills) {
      const Time dt = own_fills_[counted_fills].t - t_start_;
      const auto bin = static_cast<std::size_t>(dt / kNanosPerSecond);
      if (dt >= 0 && bin < nbins) ++fill_bins[bin];
    }
  }

  // Markouts, dispersion and realized volatility from the recorded paths.
  add_markouts(own_fills_, mid_path_, kMarkoutHorizons.data(), kMarkoutHorizons.size(),
               res_.markout_sum.data());
  res_.fill_dispersion = dispersion_index(fill_bins);
  res_.mo_dispersion = dispersion_index(mo_bins_);
  for (std::size_t j = 0; j < kVolScales.size(); ++j) {
    const auto n = static_cast<std::int64_t>(std::floor(p_.horizon / kVolScales[j] + 1e-9));
    double prev = mid_at(mid_path_, t_start_);
    double ss = 0.0;
    for (std::int64_t i = 1; i <= n; ++i) {
      const double m =
          mid_at(mid_path_, t_start_ + seconds_to_ns(kVolScales[j] * static_cast<double>(i)));
      ss += (m - prev) * (m - prev);
      prev = m;
    }
    res_.vol_ss[j] = ss;
    res_.vol_n[j] = n;
  }
  res_.mid_start = mid_path_.front().mid;
  if (log_) log_->mid_path = mid_path_;
  return res_;
}

void Market::process(const Event& e, Strategy* strategy) {
  switch (e.cls) {
    case kClsLimitBid:
    case kClsLimitAsk:
      on_limit(e);
      break;
    case kClsMarket: {
      const Side side = rng_mo_.bernoulli(0.5) ? Side::Bid : Side::Ask;
      Event n = e;
      n.t = e.t + seconds_to_ns(rng_mo_.exponential(p_.mo_rate));
      ++n.sub;
      push(n);
      const Qty lots = draw_size(rng_mo_size_, p_.mo_size_mean);
      if (log_) log_->exo.push_back({e.t, 2, side, lots, 0, 0.0});
      on_market(side, lots, false);
      break;
    }
    case kClsInformed: {
      const HawkesEvent& h = informed_[next_informed_];
      ++next_informed_;
      if (next_informed_ < informed_.size()) {
        Event n = e;
        n.t = std::max(e.t, seconds_to_ns(informed_[next_informed_].t));
        n.sub = next_informed_;
        push(n);
      }
      const Qty lots = draw_size(rng_inf_size_, p_.inf_size_mean);
      if (log_) log_->exo.push_back({e.t, 3, h.side, lots, 0, 0.0});
      on_market(h.side, lots, true);
      break;
    }
    case kClsCancel:
      if (log_) log_->exo.push_back({e.t, 1, Side::Bid, static_cast<std::int64_t>(e.id), 0, 0.0});
      book_.cancel(e.id);  // UnknownId if it already traded away: nothing to do
      break;
    case kClsAction:
      on_action(e);
      break;
    case kClsRequote:
      if (strategy) on_requote(e, *strategy);
      break;
    case kClsEnd:
      end_episode();
      break;
    default:
      break;
  }
  refresh_mid();
  ++events_;
  if (p_.check_every > 0 && events_ % p_.check_every == 0) {
    std::string why;
    if (log_) ++log_->invariant_checks;
    if (!book_.check_invariants(true, &why)) {
      ++res_.invariant_failures;
      if (log_ && log_->first_invariant_failure.empty()) log_->first_invariant_failure = why;
    }
  }
}

void Market::on_limit(const Event& e) {
  const Side side = e.cls == kClsLimitBid ? Side::Bid : Side::Ask;
  Rng& r = side == Side::Bid ? rng_lo_bid_ : rng_lo_ask_;
  const double u = r.uniform();
  const auto it = std::upper_bound(distance_cdf_.begin(), distance_cdf_.end(), u);
  const std::int64_t dist = 1 + (it - distance_cdf_.begin());
  Event n = e;
  n.t = e.t + seconds_to_ns(r.exponential(lo_total_rate_));
  ++n.sub;
  push(n);

  const Qty lots = draw_size(rng_lo_size_, p_.lo_size_mean);
  const OrderId id = ++bg_count_;
  const double life = Rng::for_stream(lifetime_seed_, id).exponential(p_.cancel_hazard);
  if (log_) log_->exo.push_back({e.t, 0, side, dist, lots, life});

  const Side opp = opposite(side);
  std::optional<Price> ref = last_best_[side_ix(opp)];
  if (const auto b = book_.best(opp)) ref = b->price;
  const Price base = ref ? *ref : p_.ref_price;
  const Price price = std::max<Price>(1, base - sign(side) * dist);
  fills_.clear();
  book_.submit_limit(id, side, price, lots * kSharesPerLot, e.t, fills_);
  if (!fills_.empty()) handle_fills(mid_);  // cannot happen: price is behind the opposite best

  Event c;
  c.t = e.t + seconds_to_ns(life);
  c.cls = kClsCancel;
  c.sub = id;
  c.id = id;
  push(c);
}

void Market::on_market(Side side, Qty lots, bool informed) {
  const double mid_before = mid_;
  fills_.clear();
  const OrderId id = kMarketIdBase + ++mo_count_;
  const SubmitResult r = book_.submit_market(id, side, lots * kSharesPerLot, t_, fills_);
  if (in_episode_) {
    ++res_.market_orders;
    if (informed) ++res_.informed_orders;
    const auto bin = static_cast<std::size_t>((t_ - t_start_) / kNanosPerSecond);
    if (bin < mo_bins_.size()) ++mo_bins_[bin];
    if (r.filled > 0 && mid_defined_) {
      const double reach =
          static_cast<double>(sign(side)) * (static_cast<double>(fills_.back().price) - mid_before);
      const long j = std::lround(2.0 * reach);
      const auto idx = static_cast<std::size_t>(std::clamp<long>(j, 0, kReachBins - 1));
      ++res_.reach_hist[idx];
    }
  }
  handle_fills(mid_before);
}

void Market::handle_fills(double mid_before) {
  for (const Fill& f : fills_) {
    if (!is_own_id(f.resting_id)) continue;
    const Side s = f.resting_side;
    const Qty lots = f.qty / kSharesPerLot;
    if (s == Side::Bid) {
      inventory_ += lots;
      cash_ -= f.price * f.qty;
      ++res_.bid_fills;
    } else {
      inventory_ -= lots;
      cash_ += f.price * f.qty;
      ++res_.ask_fills;
    }
    ++res_.fills;
    res_.fill_lots += lots;
    res_.max_abs_inventory = std::max<Qty>(res_.max_abs_inventory, std::abs(inventory_));
    const double sgn = s == Side::Bid ? 1.0 : -1.0;
    res_.spread_captured +=
        static_cast<double>(lots) * sgn * (mid_before - static_cast<double>(f.price));
    const OwnFill of{f.ts, s, f.price, lots, mid_before};
    own_fills_.push_back(of);
    if (log_) {
      log_->fills.push_back(of);
      log_->raw_fills.push_back(f);
      log_->inventory_path.emplace_back(f.ts, inventory_);
    }
    for (std::size_t i = 0; i < own_.size(); ++i) {
      if (own_[i].id != f.resting_id) continue;
      own_[i].shares -= f.qty;
      if (f.resting_left == 0) close_own(i);
      break;
    }
    Working& w = working_[side_ix(s)];
    if (w.active && w.id == f.resting_id) {
      w.lots -= lots;
      if (f.resting_left == 0) w = Working{};
    }
    if (strategy_) strategy_->on_fill(of);
  }
}

void Market::close_own(std::size_t idx) {
  own_[idx] = own_.back();
  own_.pop_back();
}

Qty Market::own_resting_lots(Side s) const {
  Qty q = 0;
  for (const OwnLive& o : own_) {
    if (o.side == s) q += o.shares / kSharesPerLot;
  }
  return q;
}

void Market::on_requote(const Event& e, Strategy& strategy) {
  if (!trading_) return;
  StrategyView v;
  v.t = static_cast<double>(t_ - t_start_) / static_cast<double>(kNanosPerSecond);
  v.horizon = p_.horizon;
  if (const auto b = book_.best_bid()) v.best_bid = b->price;
  if (const auto a = book_.best_ask()) v.best_ask = a->price;
  if (others_[0]) v.others_bid = others_[0]->price;
  if (others_[1]) v.others_ask = others_[1]->price;
  v.mid = mid_;
  v.mid_defined = mid_defined_;
  v.inventory = inventory_;
  v.max_inventory = p_.max_inventory;
  v.quote_size = p_.quote_size;
  for (std::size_t s = 0; s < 2; ++s) {
    OwnOrderView& o = s == 0 ? v.own_bid : v.own_ask;
    o.active = working_[s].active;
    o.live = working_[s].live;
    o.price = working_[s].price;
    o.lots = working_[s].lots;
  }
  v.book = &book_;
  v.rng = &rng_strategy_;
  const QuoteTarget target = strategy.quote(v);

  const Time effect = t_ + seconds_to_ns(p_.latency);
  bool keep[2];
  for (std::size_t s = 0; s < 2; ++s) {
    const bool on = s == 0 ? target.bid_on : target.ask_on;
    const Price px = s == 0 ? target.bid : target.ask;
    const Working& w = working_[s];
    keep[s] = w.active && on && w.price == px && w.lots == target.lots;
  }
  // Cancels first, then submits, so that at the effect time the old orders are gone.
  for (std::size_t s = 0; s < 2; ++s) {
    if (keep[s] || !working_[s].active) continue;
    Event c;
    c.t = effect;
    c.cls = kClsAction;
    c.sub = ++action_count_;
    c.action = kActCancel;
    c.id = working_[s].id;
    c.side = s == 0 ? Side::Bid : Side::Ask;
    push(c);
    working_[s] = Working{};
  }
  for (std::size_t s = 0; s < 2; ++s) {
    const bool on = s == 0 ? target.bid_on : target.ask_on;
    if (keep[s] || !on || target.lots <= 0) continue;
    Event a;
    a.t = effect;
    a.cls = kClsAction;
    a.sub = ++action_count_;
    a.action = kActSubmit;
    a.id = kOwnIdBase + ++own_count_;
    a.side = s == 0 ? Side::Bid : Side::Ask;
    a.price = s == 0 ? target.bid : target.ask;
    a.lots = target.lots;
    push(a);
    working_[s] = Working{true, false, a.id, a.price, a.lots};
  }

  ++wake_count_;
  const Time next = t_start_ + seconds_to_ns(p_.requote * static_cast<double>(wake_count_));
  if (next < t_end_) {
    Event n = e;
    n.t = next;
    n.sub = wake_count_;
    push(n);
  }
}

void Market::on_action(const Event& e) {
  if (!trading_) return;
  if (e.action == kActCancel) {
    if (book_.cancel(e.id) == Status::Ok) {
      ++res_.cancels;
      for (std::size_t i = 0; i < own_.size(); ++i) {
        if (own_[i].id == e.id) {
          close_own(i);
          break;
        }
      }
    }
    return;
  }
  const Side s = e.side;
  const auto opp = book_.best(opposite(s));
  const Qty resting = own_resting_lots(s);
  SubmitRecord rec{t_, s, e.price, std::nullopt, inventory_, resting, false};
  if (opp) rec.opposite_best = opp->price;
  const bool crosses = opp && (s == Side::Bid ? e.price >= opp->price : e.price <= opp->price);
  const Qty exposure = (s == Side::Bid ? inventory_ : -inventory_) + resting + e.lots;
  Working& w = working_[side_ix(s)];
  if (crosses || e.price < 1) {
    ++res_.post_only_rejects;
  } else if (exposure > p_.max_inventory) {
    ++res_.limit_rejects;
  } else {
    fills_.clear();
    book_.submit_limit(e.id, s, e.price, e.lots * kSharesPerLot, t_, fills_);
    own_.push_back({e.id, s, e.price, e.lots * kSharesPerLot, t_});
    ++res_.submits;
    rec.accepted = true;
    if (w.active && w.id == e.id) w.live = true;
  }
  if (!rec.accepted && w.active && w.id == e.id) w = Working{};
  if (log_) log_->submits.push_back(rec);
}

void Market::end_episode() {
  trading_ = false;
  for (const OwnLive& o : own_) {
    if (book_.cancel(o.id) == Status::Ok) ++res_.cancels;
  }
  own_.clear();
  working_[0] = Working{};
  working_[1] = Working{};
  refresh_mid();
  res_.pnl = static_cast<double>(cash_) * kUsdPerTick +
             static_cast<double>(inventory_ * kSharesPerLot) * mid_ * kUsdPerTick;
  res_.final_inventory = inventory_;
  res_.mid_end = mid_;
  if (log_) {
    log_->final_cash = cash_;
    log_->final_inventory = inventory_;
    log_->final_mid = mid_;
  }
}

void Market::advance_to(Time t) {
  const Time lo = std::max(t_, t_start_);
  const Time hi = std::min(t, t_end_);
  if (consumed_ && hi > lo) {
    const double dt = static_cast<double>(hi - lo) / static_cast<double>(kNanosPerSecond);
    if (others_[0] && others_[1]) {
      res_.spread_time += static_cast<double>(others_[1]->price - others_[0]->price) * dt;
      res_.touch_depth_time +=
          0.5 * static_cast<double>(others_[0]->qty + others_[1]->qty) / kSharesPerLot * dt;
      res_.two_sided_time += dt;
    }
    res_.abs_inventory_time += static_cast<double>(std::abs(inventory_)) * dt;
    for (const OwnLive& o : own_) {
      res_.quote_seconds += dt;
      const double d = o.side == Side::Bid ? mid_ - static_cast<double>(o.price)
                                           : static_cast<double>(o.price) - mid_;
      res_.quote_dist_time += d * dt;
    }
  }
  t_ = t;
  in_episode_ = consumed_ && t_ >= t_start_ && t_ < t_end_;
}

std::optional<Level> Market::others_best(Side s) {
  std::size_t n_own = 0;
  for (const OwnLive& o : own_) n_own += o.side == s ? 1 : 0;
  book_.depth(s, n_own + 1, depth_buf_);
  for (const Level& lv : depth_buf_) {
    Qty q = lv.qty;
    for (const OwnLive& o : own_) {
      if (o.side == s && o.price == lv.price) q -= o.shares;
    }
    if (q > 0) return Level{lv.price, q, lv.count};
  }
  return std::nullopt;
}

void Market::refresh_mid() {
  if (const auto b = book_.best_bid()) last_best_[0] = b->price;
  if (const auto a = book_.best_ask()) last_best_[1] = a->price;
  others_[0] = others_best(Side::Bid);
  others_[1] = others_best(Side::Ask);
  if (others_[0] && others_[1]) {
    const double m = 0.5 * static_cast<double>(others_[0]->price + others_[1]->price);
    mid_defined_ = true;
    if (m != mid_) {
      mid_ = m;
      if (consumed_ && t_ >= t_start_) mid_path_.push_back({t_, mid_});
    }
  } else {
    mid_defined_ = false;
    if (mid_ == 0.0) mid_ = static_cast<double>(p_.ref_price);
  }
}

double mid_at(const std::vector<MidPoint>& path, Time t) {
  const auto it = std::upper_bound(path.begin(), path.end(), t,
                                   [](Time x, const MidPoint& m) { return x < m.t; });
  if (it == path.begin()) return path.front().mid;
  return std::prev(it)->mid;
}

void add_markouts(const std::vector<OwnFill>& fills, const std::vector<MidPoint>& path,
                  const double* horizons_s, std::size_t n_horizons, double* sums) {
  for (const OwnFill& f : fills) {
    const double sgn = f.side == Side::Bid ? 1.0 : -1.0;
    for (std::size_t j = 0; j < n_horizons; ++j) {
      const double m = mid_at(path, f.t + seconds_to_ns(horizons_s[j]));
      sums[j] += static_cast<double>(f.lots) * sgn * (m - static_cast<double>(f.price));
    }
  }
}

double dispersion_index(const std::vector<std::int64_t>& counts) {
  const std::size_t n = counts.size();
  if (n < 2) return std::numeric_limits<double>::quiet_NaN();
  double sum = 0.0;
  for (std::int64_t c : counts) sum += static_cast<double>(c);
  const double mean = sum / static_cast<double>(n);
  if (!(mean > 0.0)) return std::numeric_limits<double>::quiet_NaN();
  double ss = 0.0;
  for (std::int64_t c : counts) ss += (static_cast<double>(c) - mean) * (static_cast<double>(c) - mean);
  return ss / static_cast<double>(n - 1) / mean;
}

}  // namespace lob::backtest
