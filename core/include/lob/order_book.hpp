// lob::OrderBook: single-threaded limit order book with price-time priority.
//
// ------------------------------------------------------------------------------------------------
// MODES. One book, two sets of mutating calls (see docs/design.md, "Core order book contract"):
//   Matching mode (simulator): submit_limit, submit_market, cancel, reduce, replace. Incoming
//     orders trade against the opposite side, best price first and oldest first within a price,
//     always at the RESTING order's price. Used on its own, this keeps best bid < best ask.
//   Replay mode (LOBSTER): add_resting, execute_resting, cancel, reduce. Nothing matches; the book
//     may become locked or crossed because the data says so.
//   The calls may be mixed (for example, a backtest that replays market data and submits its own
//   orders). If add_resting has crossed the book, a later submit_limit still matches only against
//   the opposite side and its remainder rests, so the pre-existing cross persists.
//
// FILLS. Every call that can trade appends Fill records to a caller-owned std::vector<Fill>; it
// never clears the vector. Reuse one vector (clear() it yourself between calls) and it stops
// allocating once its capacity covers the largest sweep; reserve() it to avoid even that.
// Fills are appended in execution order. A zero-size fill is never reported.
//
// DATA STRUCTURES (and why).
//   Order pool: std::vector<Node> plus a free list of slot indices. Nodes are recycled, so steady
//     state does no heap allocation, and 32-bit indices instead of pointers survive vector growth.
//   Price level: aggregate size, order count and an intrusive doubly linked FIFO of pool indices
//     (prev/next live in the node). O(1) append at the tail, O(1) removal anywhere.
//   Levels per side: std::vector sorted with the best price at the BACK. Best price is back(),
//     emptying the best level is pop_back(), and inserting/erasing a level k places from the touch
//     moves only k elements. Finding a level scans up to 8 levels from the touch, then binary
//     searches. The alternative std::map container (CMake option LOB_CORE_MAP_LEVELS) exists only
//     for the comparison in core/README.md.
//   Id index: open-addressing hash map id -> pool index (linear probing, backward-shift deletion,
//     load factor <= 1/2). O(1) expected.
//
// COMPLEXITY. L = number of price levels on the touched side, d = distance in levels between the
// touched price and the best price (d <= L, usually small), k = orders filled, m = levels emptied.
//   submit_limit / submit_market    O(k + m) for matching + O(min(d, 8) + log L + d) to rest
//   add_resting                     O(min(d, 8) + log L) to find the level, plus O(d) to insert a
//                                   new level (vector shift)
//   cancel, reduce, execute_resting O(1) expected id lookup + level lookup O(min(d, 8) + log L)
//                                   (needed to update the level's aggregate size), plus O(d) if the
//                                   order was the last one at its level
//   replace                         keeps priority: same as reduce;
//                                   loses priority: cancel + submit_limit
//   best_bid/best_ask/mid/spread    O(1)
//   front(side), find(id)           O(1) expected
//   depth(side, n)                  O(n)
//   queue_ahead(id)                 O(position of the order in its level)   <- not O(1)
//   order_count, level_count, total_qty   O(1)
//   check_invariants                O(orders + levels + index capacity)
// Hash-table growth and vector growth are amortized O(1); reserve() removes them.
//
// ERRORS. Mutating calls never throw on bad input; they return a Status and leave the book
// unchanged when the status is not Ok. Checked in this order:
//   id == kNoOrder (0)            -> InvalidId       (0 is the "no order" sentinel)
//   price <= 0                    -> InvalidPrice    (limit/resting/replace prices)
//   qty <= 0                      -> InvalidQty
//   new order id already resting  -> DuplicateId     (also for market orders, so fills stay
//                                                     unambiguous; ids are not otherwise
//                                                     remembered after an order leaves the book)
//   resting size would push the   -> InvalidQty      (submit_limit, add_resting, and replace
//   side's total past INT64_MAX                       when it loses priority; submit_limit checks
//                                                     its whole size before matching)
//   id not resting                -> UnknownId       (cancel, reduce, replace, execute_resting)
// Nothing is thrown except std::bad_alloc if growing the pool, index or level vectors runs out of
// memory. Precondition: fewer than 2^32 - 1 orders rest at once (pool slots are 32-bit indices).
//
// OTHER EDGE CASES.
//   reduce(id, by) with by >= remaining removes the order (contract: "removes the order if
//     nothing remains"); execute_resting(id, q) with q >= remaining fills only what remains and
//     removes the order. Neither ever leaves a zero-size order in the book.
//   submit_market with too little liquidity fills what exists; the rest is discarded
//     (result.filled < qty, result.resting == 0). An empty opposite side is not an error.
//   replace(id, price, new_qty): new_qty is the new REMAINING size. Priority is kept iff the price
//     is unchanged and new_qty <= current remaining size; the timestamp is then unchanged too.
//     Otherwise the order is pulled and resubmitted with the same id at timestamp `ts`; it can
//     match (fills carry aggressor_id = id) and any remainder joins the back of its level.
//   No self-trade prevention: orders carry no owner.
// ------------------------------------------------------------------------------------------------
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "lob/book_detail/id_index.hpp"
#include "lob/book_detail/price_levels.hpp"
#include "lob/types.hpp"

namespace lob {

enum class Status : std::uint8_t {
  Ok = 0,
  InvalidId,     // id == kNoOrder
  InvalidPrice,  // price <= 0
  InvalidQty,    // qty <= 0
  DuplicateId,   // id already resting in the book
  UnknownId,     // id not resting in the book
};

const char* to_string(Status s) noexcept;

// One trade between a resting order and an aggressor.
struct Fill {
  OrderId resting_id = kNoOrder;
  OrderId aggressor_id = kNoOrder;  // kNoOrder for execute_resting (external aggressor)
  Side resting_side = Side::Bid;
  Price price = 0;                  // always the resting order's price
  Qty qty = 0;                      // > 0
  Time ts = 0;                      // timestamp of the aggressing call
  Qty resting_left = 0;             // resting order's remaining size after this fill (0 = gone)
};

// Aggregated price level as seen from outside.
struct Level {
  Price price = 0;
  Qty qty = 0;               // sum of remaining sizes
  std::uint32_t count = 0;   // number of orders
};

// A resting order as seen from outside.
struct OrderInfo {
  OrderId id = kNoOrder;
  Side side = Side::Bid;
  Price price = 0;
  Qty qty = 0;  // remaining size
  Time ts = 0;  // time the order took its current queue position
};

// Outcome of a call that can trade.
struct SubmitResult {
  Status status = Status::Ok;
  Qty filled = 0;   // total size traded by the incoming order
  Qty resting = 0;  // size left resting in the book under this id (0 for market orders)
};

class OrderBook {
 public:
  OrderBook();

  // Preallocate for up to `max_orders` resting orders and `max_levels_per_side` levels per side,
  // so that steady-state operation below those sizes never allocates. Does not change contents.
  void reserve(std::size_t max_orders, std::size_t max_levels_per_side);

  // Remove every order. Keeps allocated capacity.
  void clear();

  // ---- Matching mode --------------------------------------------------------------------------

  // Limit order: trades against the opposite side while its best price is at least as good as
  // `price`; the remainder rests at `price` with timestamp `ts`.
  SubmitResult submit_limit(OrderId id, Side side, Price price, Qty qty, Time ts,
                            std::vector<Fill>& fills);

  // Market order: sweeps the opposite side up to `qty`; the unfilled part is discarded.
  SubmitResult submit_market(OrderId id, Side side, Qty qty, Time ts, std::vector<Fill>& fills);

  // Change price and/or remaining size; see the priority rule in the header comment. `ts` is used
  // only if the order loses priority (new timestamp, and timestamp of any fills).
  SubmitResult replace(OrderId id, Price new_price, Qty new_qty, Time ts, std::vector<Fill>& fills);

  // ---- Both modes ------------------------------------------------------------------------------

  // Remove the order entirely.
  Status cancel(OrderId id);

  // Partial cancel that keeps queue position; removes the order if by_qty >= remaining.
  Status reduce(OrderId id, Qty by_qty);

  // ---- Replay mode -----------------------------------------------------------------------------

  // Rest an order without matching. May lock or cross the book.
  Status add_resting(OrderId id, Side side, Price price, Qty qty, Time ts);

  // Execute min(qty, remaining) of a resting order against an external aggressor; appends one
  // Fill with aggressor_id = kNoOrder. Keeps queue position if size remains.
  Status execute_resting(OrderId id, Qty qty, Time ts, std::vector<Fill>& fills);

  // ---- Queries ---------------------------------------------------------------------------------

  std::optional<Level> best(Side side) const noexcept {
    const auto& lv = levels(side);
    if (lv.empty()) return std::nullopt;
    const book_detail::LevelQueue& b = lv.best();
    return Level{b.price, b.qty, b.count};
  }
  std::optional<Level> best_bid() const noexcept { return best(Side::Bid); }
  std::optional<Level> best_ask() const noexcept { return best(Side::Ask); }

  // (best bid + best ask) / 2 in ticks; empty if either side is empty.
  std::optional<double> mid() const noexcept;

  // best ask - best bid in ticks; empty if either side is empty. Zero or negative only if the
  // book is locked or crossed (replay mode).
  std::optional<Price> spread() const noexcept;

  // Oldest order at the best price of `side` (the next one to be hit).
  std::optional<OrderInfo> front(Side side) const noexcept;

  // Writes the best min(n, level_count(side)) levels of `side`, best first, into `out` (which is
  // cleared first). Allocation-free once `out` has capacity n.
  void depth(Side side, std::size_t n, std::vector<Level>& out) const;

  std::optional<OrderInfo> find(OrderId id) const noexcept;

  // Total size of the orders ahead of `id` in its price level (0 if it is at the front).
  std::optional<Qty> queue_ahead(OrderId id) const noexcept;

  std::size_t order_count() const noexcept { return index_.size(); }
  std::size_t level_count(Side side) const noexcept { return levels(side).size(); }
  Qty total_qty(Side side) const noexcept { return total_qty_[side_index(side)]; }
  bool empty() const noexcept { return index_.size() == 0; }

  // Verify every structural invariant: per-level size and count equal the sum over its queue,
  // queue links are consistent, each order sits on its own side and price, levels are strictly
  // sorted and non-empty, the id index matches the queues exactly, the free list accounts for
  // every other pool slot, and the per-side totals match. If `require_uncrossed`, also requires
  // best bid < best ask (pass false in replay mode). Returns true if all hold; otherwise false and,
  // if `why` is not null, a description of the first violation.
  bool check_invariants(bool require_uncrossed = true, std::string* why = nullptr) const;

 private:
  struct Node {
    OrderId id = kNoOrder;
    Price price = 0;
    Qty qty = 0;
    Time ts = 0;
    std::uint32_t prev = book_detail::kNil;
    std::uint32_t next = book_detail::kNil;
    Side side = Side::Bid;
  };

  static constexpr std::size_t side_index(Side s) noexcept { return s == Side::Bid ? 0 : 1; }
  book_detail::PriceLevels& levels(Side s) noexcept { return levels_[side_index(s)]; }
  const book_detail::PriceLevels& levels(Side s) const noexcept {
    return levels_[side_index(s)];
  }

  static Status validate_new(OrderId id, Price price, Qty qty) noexcept;
  bool would_overflow(Side side, Qty qty) const noexcept;
  std::uint32_t allocate_node();
  void rest(OrderId id, Side side, Price price, Qty qty, Time ts);
  Qty match(OrderId aggressor, Side side, bool has_limit, Price limit, Qty qty, Time ts,
            std::vector<Fill>& fills);
  void remove_node(std::uint32_t n);
  void shrink_node(std::uint32_t n, Qty by);

  std::vector<Node> pool_;
  std::uint32_t free_head_ = book_detail::kNil;  // free list threaded through Node::next
  book_detail::IdIndex index_;
  book_detail::PriceLevels levels_[2];
  Qty total_qty_[2] = {0, 0};
};

}  // namespace lob
