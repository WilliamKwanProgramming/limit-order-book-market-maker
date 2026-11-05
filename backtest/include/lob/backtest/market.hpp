// Event-driven market simulator on top of lob::OrderBook (matching mode).
//
// TIMELINE of one episode (seconds): [0, B) burn-in, [B, B + T) episode, [B + T, B + T + tail)
// markout tail. The book starts with init_levels levels of init_lots lots per side around
// ref_price; burn-in lets the background flow replace it (mean lifetime 1/theta << B).
// Our strategy is active only during the episode; at B + T all our orders are cancelled and the
// episode PnL is marked at the others' mid. The tail runs the market without us, only so that
// fills near T get their 10 s markout.
//
// EXOGENOUS EVENTS and their random streams (Rng::for_stream(episode_seed, id), ids in market.cpp):
//   background limit orders, per side: Poisson, total rate lambda_0 sum_{i=1..K} i^-alpha; the
//     distance i from the opposite best price is drawn with probability proportional to i^-alpha
//     (bid at best_ask - i, ask at best_bid + i; the last known best if that side is empty);
//     size from the limit-size stream; lifetime Exponential(theta) drawn from
//     Rng::for_stream(lifetime_seed, creation index), so it depends only on the creation count.
//   background market orders: Poisson(mu), side Bernoulli(1/2), size from the market-size stream.
//   informed market orders: Hawkes clusters (hawkes.hpp) generated for [0, B + T + tail) at
//     construction from the informed-time stream, sizes from the informed-size stream.
// No exogenous draw depends on the book or on our orders, so the exogenous event sequence (times,
// sides, distances, sizes, lifetimes) is identical for every strategy run on the same seed
// (common random numbers). Its effect on the book is not: a background limit order's price is
// relative to the current best price, which our quotes can change, and an order cancels only if it
// has not traded.
//
// EVENT ORDER. Events are processed by (time, class, per-class counter); classes in increasing
// order: background bid, background ask, background market, informed market, background cancel,
// our order action, our requote, end of episode. All exogenous keys are exogenous, so our events
// never change the relative order of exogenous ones.
//
// OUR ORDERS rest in the same book with real price-time priority and ids >= kOwnIdBase. At a
// requote the order manager compares the strategy's target with the working order of each side:
// same price and same remaining size -> keep (queue position kept); otherwise cancel it and
// submit a new order. Cancels and submits take effect `latency` seconds later (cancels first).
// When a submit reaches the book it is REJECTED (counted, not sent) if it would cross the public
// book (post-only) or if inventory plus all our resting size on that side plus the new size would
// exceed Q_max (pre-trade risk check), so |inventory| <= Q_max at all times.
//
// MID. "Mid" means the others' mid: the mid of the book with our own orders removed, carried
// forward if a side is empty. Strategies, mark-to-market, markouts and realized volatility all use
// it, so our own quotes never move the price we are measured against.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "lob/backtest/hawkes.hpp"
#include "lob/backtest/params.hpp"
#include "lob/backtest/strategy.hpp"
#include "lob/order_book.hpp"
#include "lob/rng.hpp"

namespace lob::backtest {

// Market-order reach histogram, in half ticks from the mid (index j <-> reach j/2 ticks).
inline constexpr std::size_t kReachBins = 64;
// Sampling intervals (seconds) for realized volatility.
inline constexpr std::array<double, 4> kVolScales = {0.1, 1.0, 10.0, 60.0};
// Markout horizons (seconds).
inline constexpr std::array<double, 3> kMarkoutHorizons = {0.1, 1.0, 10.0};

struct MidPoint {
  Time t = 0;
  double mid = 0.0;
};

// Per-episode output. Sums are kept (not means) so that pooled ratios can be formed.
struct EpisodeResult {
  // PnL and inventory (our strategy).
  double pnl = 0.0;               // $: cash + inventory x others' mid at T
  Qty final_inventory = 0;        // lots
  double abs_inventory_time = 0;  // lot-seconds: integral of |inventory| over the episode
  Qty max_abs_inventory = 0;      // lots
  // Fills.
  std::int64_t fills = 0;         // engine Fill records on our orders
  std::int64_t fill_lots = 0;
  std::int64_t bid_fills = 0;
  std::int64_t ask_fills = 0;
  double quote_seconds = 0.0;     // sum over our orders of the time they rested in the book
  double quote_dist_time = 0.0;   // integral of (distance of a resting quote from mid) dt, ticks*s
  double spread_captured = 0.0;   // sum over fills of lots * signed (mid_before - price), ticks*lots
  std::array<double, kMarkoutHorizons.size()> markout_sum{};  // ticks*lots
  double fill_dispersion = 0.0;   // variance/mean of our fills per 1 s bin (NaN if no fills)
  // Order-manager counters.
  std::int64_t submits = 0;
  std::int64_t cancels = 0;
  std::int64_t post_only_rejects = 0;
  std::int64_t limit_rejects = 0;
  // Market diagnostics over the episode window (others' book).
  double spread_time = 0.0;       // integral of others' spread (ticks*s) while both sides exist
  double touch_depth_time = 0.0;  // integral of mean others' touch depth (lots*s)
  double two_sided_time = 0.0;    // s with both sides non-empty
  std::int64_t market_orders = 0; // background + informed, during the episode
  std::int64_t informed_orders = 0;
  double mo_dispersion = 0.0;     // variance/mean of all market orders per 1 s bin
  std::array<double, kVolScales.size()> vol_ss{};  // sum of squared mid increments (ticks^2)
  std::array<std::int64_t, kVolScales.size()> vol_n{};
  std::array<std::int64_t, kReachBins> reach_hist{};  // market orders by reach (half ticks)
  double mid_start = 0.0;
  double mid_end = 0.0;
  std::int64_t invariant_failures = 0;
};

// Optional detailed record of an episode, for tests.
struct ExoRecord {
  Time t = 0;
  std::uint8_t kind = 0;  // 0 limit, 1 cancel, 2 market, 3 informed
  Side side = Side::Bid;
  std::int64_t a = 0;     // limit: distance; cancel: order id; market: lots
  std::int64_t b = 0;     // limit: lots
  double c = 0.0;         // limit: lifetime (s)
  bool operator==(const ExoRecord& o) const {
    return t == o.t && kind == o.kind && side == o.side && a == o.a && b == o.b && c == o.c;
  }
};

struct SubmitRecord {
  Time t = 0;
  Side side = Side::Bid;
  Price price = 0;
  std::optional<Price> opposite_best;  // public book at the moment of submission
  Qty inventory = 0;                   // lots, before
  Qty resting_same_side = 0;           // lots, our resting size on that side before
  bool accepted = false;
};

struct EpisodeLog {
  bool record_exogenous = true;
  std::vector<ExoRecord> exo;              // every exogenous event from the episode start on
  std::vector<OwnFill> fills;              // our fills, as attributed by the simulator
  std::vector<Fill> raw_fills;             // every engine Fill touching one of our ids
  std::vector<MidPoint> mid_path;          // others' mid change points from the episode start
  std::vector<SubmitRecord> submits;
  std::vector<std::pair<Time, Qty>> inventory_path;  // after each change, lots
  std::int64_t invariant_checks = 0;
  std::string first_invariant_failure;
  std::int64_t final_cash = 0;             // tick-shares
  Qty final_inventory = 0;                 // lots
  double final_mid = 0.0;
};

class Market {
 public:
  Market(const SimParams& p, std::uint64_t episode_seed);

  // Runs the burn-in [0, B). Call once, then copy the Market for each strategy.
  void burn_in();

  // Runs the episode and the markout tail with `strategy`. Call on a burned-in Market (or a copy);
  // the Market is consumed (it cannot run a second episode).
  EpisodeResult run_episode(Strategy& strategy, EpisodeLog* log = nullptr);

  const OrderBook& book() const noexcept { return book_; }
  const std::vector<HawkesEvent>& informed_events() const noexcept { return informed_; }

 private:
  struct Event {
    Time t = 0;
    std::uint64_t sub = 0;
    std::uint8_t cls = 0;
    std::uint8_t action = 0;  // for our actions: 0 cancel, 1 submit
    Side side = Side::Bid;
    OrderId id = kNoOrder;
    Price price = 0;
    Qty lots = 0;
  };
  struct Later {
    bool operator()(const Event& a, const Event& b) const noexcept {
      if (a.t != b.t) return a.t > b.t;
      if (a.cls != b.cls) return a.cls > b.cls;
      return a.sub > b.sub;
    }
  };
  struct OwnLive {
    OrderId id;
    Side side;
    Price price;
    Qty shares;
    Time rested_at;
  };
  struct Working {
    bool active = false;
    bool live = false;
    OrderId id = kNoOrder;
    Price price = 0;
    Qty lots = 0;
  };

  void push(const Event& e);
  Event pop();
  void process(const Event& e, Strategy* strategy);
  void on_limit(const Event& e);
  void on_market(Side side, Qty lots, bool informed);
  void on_requote(const Event& e, Strategy& strategy);
  void on_action(const Event& e);
  void handle_fills(double mid_before);
  void close_own(std::size_t idx);
  void end_episode();
  void advance_to(Time t);
  void refresh_mid();
  std::optional<Level> others_best(Side s);
  Qty own_resting_lots(Side s) const;
  Qty draw_size(Rng& r, double mean) const;

  SimParams p_;
  OrderBook book_;
  std::vector<Event> heap_;
  std::vector<Fill> fills_;
  std::vector<Level> depth_buf_;

  Rng rng_lo_bid_, rng_lo_ask_, rng_lo_size_, rng_mo_, rng_mo_size_, rng_inf_size_, rng_strategy_;
  std::uint64_t lifetime_seed_;
  std::vector<double> distance_cdf_;
  double lo_total_rate_ = 0.0;
  std::vector<HawkesEvent> informed_;
  std::size_t next_informed_ = 0;

  OrderId bg_count_ = 0;
  OrderId mo_count_ = 0;
  OrderId own_count_ = 0;
  std::uint64_t action_count_ = 0;
  std::uint64_t wake_count_ = 0;
  Time t_ = 0;
  Time t_start_ = 0, t_end_ = 0, t_tail_ = 0;
  bool in_episode_ = false;  // t in [t_start_, t_end_)
  bool trading_ = false;
  bool consumed_ = false;

  std::optional<Price> last_best_[2];
  double mid_ = 0.0;
  bool mid_defined_ = false;
  std::optional<Level> others_[2];

  // Our state.
  std::vector<OwnLive> own_;
  Working working_[2];
  std::int64_t cash_ = 0;   // tick-shares
  Qty inventory_ = 0;       // lots
  Strategy* strategy_ = nullptr;

  EpisodeResult res_;
  EpisodeLog* log_ = nullptr;
  std::vector<OwnFill> own_fills_;
  std::vector<MidPoint> mid_path_;
  std::vector<std::int64_t> mo_bins_;
  std::int64_t events_ = 0;
};

// Markout of each fill at each horizon h: lots * sign * (mid(t + h) - price) in ticks, sign = +1
// for a buy and -1 for a sell, so a price move against us after the fill is negative. mid(t) is
// the value of the last point of `path` with time <= t (path sorted by time, non-empty, first
// point at or before every fill). Adds to sums[j] for horizon j.
void add_markouts(const std::vector<OwnFill>& fills, const std::vector<MidPoint>& path,
                  const double* horizons_s, std::size_t n_horizons, double* sums);

// Value of a step-function path at t (last point with time <= t; the first point if t is earlier).
double mid_at(const std::vector<MidPoint>& path, Time t);

// Sample variance / mean of counts (NaN if the mean is 0 or fewer than 2 bins).
double dispersion_index(const std::vector<std::int64_t>& counts);

// Seed of episode `e` of an experiment with master seed `seed`.
std::uint64_t episode_seed(std::uint64_t seed, std::uint64_t e);

}  // namespace lob::backtest
