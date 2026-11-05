// Replays LOBSTER messages into lob::OrderBook (replay mode) and cross-checks every message.
//
// MAPPING.
//   type 1 (new limit order)        -> add_resting(id, side, price, size, time)
//   type 2 (partial cancellation)   -> reduce(id, size)
//   type 3 (deletion)               -> cancel(id)
//   type 4 (visible execution)      -> execute_resting(id, size, time)
//   types 5, 6, 7                   -> no change to the book (counted only)
//   direction +1 = Side::Bid, -1 = Side::Ask; for types 2-4 it is the side of the resting order.
//
// ANOMALIES (one counter per kind; a message may raise several).
//   UnknownId                  type 2/3/4 id not resting (and, with bootstrap, not explained by a
//                              ghost order)
//   DuplicateId                type 1 id already resting
//   SizeExceedsRemaining       type 2/4 size larger than the remaining size (with bootstrap: the
//                              ghost orders at that price hold less than the message size)
//   PartialCancelRemovesOrder  type 2 size equal to the remaining size (a "partial" cancellation
//                              that removes the whole order)
//   SideMismatch               type 2/3/4 direction differs from the resting order's side
//   PriceMismatch              type 2/3/4 price differs from the resting order's price
//   DeleteSizeMismatch         type 3 size differs from the remaining size
//   TimeBackwards              time earlier than the previous message's time
//
// POLICIES.
//   Strict (default): the first message with an anomaly is not applied, and the replayer stops
//     (stopped() becomes true and later calls do nothing). first_anomaly() records it.
//   Lenient: count and continue. The engine call is skipped when the engine would reject it
//     (unknown id, duplicate id) and made otherwise: an oversized reduce or execution is capped by
//     the engine at the remaining size, and a mismatched side, price or delete size is ignored
//     because the engine works by id.
//
// BOOTSTRAP (heuristic, off by default; untested on real data). A LOBSTER window that starts after
// the open can reference orders submitted before it, which never appear as type 1. seed_ghosts()
// takes the first message and the first order-book row, undoes the message on that row to get the
// book before it, and rests one synthetic "ghost" order per visible level (id with the top bit set)
// holding the level's whole size. A type 2/3/4 message whose id is unknown is then applied to the
// ghost orders resting at the message's side and price, oldest first: it reduces (types 2 and 3)
// or executes (type 4) the message size. Only the visible N levels can be seeded; levels deeper
// than N at the start of the window stay unknown.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "lob/lobster_format.hpp"
#include "lob/order_book.hpp"

namespace lob::lobster {

enum class Policy : std::uint8_t { Strict, Lenient };
const char* to_string(Policy p) noexcept;

enum class Anomaly : std::uint8_t {
  UnknownId = 0,
  DuplicateId,
  SizeExceedsRemaining,
  PartialCancelRemovesOrder,
  SideMismatch,
  PriceMismatch,
  DeleteSizeMismatch,
  TimeBackwards,
};
inline constexpr std::size_t kAnomalyKinds = 8;
const char* to_string(Anomaly a) noexcept;  // snake_case name, used as a summary key

using AnomalyMask = std::uint32_t;
constexpr AnomalyMask bit(Anomaly a) noexcept { return AnomalyMask{1} << static_cast<unsigned>(a); }

// Ghost order ids have the top bit set; LOBSTER ids parsed as positive int64 never do.
inline constexpr OrderId kGhostIdBit = OrderId{1} << 63;
constexpr bool is_ghost(OrderId id) noexcept { return (id & kGhostIdBit) != 0; }

struct ReplayCounters {
  std::uint64_t messages = 0;                 // messages passed to apply() and processed
  std::array<std::uint64_t, 8> by_type{};     // index = message type (1..7)
  std::array<std::uint64_t, kAnomalyKinds> anomalies{};
  std::uint64_t messages_with_anomaly = 0;
  std::uint64_t skipped_engine_calls = 0;     // lenient: engine call not made
  std::uint64_t engine_rejections = 0;        // engine returned non-Ok (should stay 0)

  std::uint64_t total_anomalies() const noexcept {
    std::uint64_t s = 0;
    for (std::uint64_t a : anomalies) s += a;
    return s;
  }
  std::uint64_t field_mismatches() const noexcept {
    return anomalies[static_cast<std::size_t>(Anomaly::SideMismatch)] +
           anomalies[static_cast<std::size_t>(Anomaly::PriceMismatch)] +
           anomalies[static_cast<std::size_t>(Anomaly::DeleteSizeMismatch)];
  }
};

struct GhostCounters {
  bool enabled = false;
  bool seeded = false;
  bool first_message_undone = true;     // false if the first message could not be undone on row 1
  std::uint64_t created = 0;            // ghost orders created
  Qty created_qty = 0;
  std::uint64_t references_resolved = 0;  // unknown-id messages applied to ghost orders
  Qty consumed_qty = 0;                 // ghost size removed by those messages
  std::uint64_t emptied = 0;            // ghost orders fully consumed
  std::uint64_t unexplained = 0;        // unknown-id messages with no ghost at their side and price
  std::uint64_t shortfall = 0;          // resolved messages larger than the ghost size available
};

struct FirstAnomaly {
  bool set = false;
  std::uint64_t index = 0;  // 1-based message number
  std::uint64_t line = 0;   // line in the message file
  Message message;
  AnomalyMask mask = 0;
};

struct ReplayConfig {
  Policy policy = Policy::Strict;
  bool bootstrap = false;
};

class Replayer {
 public:
  using Config = ReplayConfig;

  explicit Replayer(Config cfg = Config{});

  // Bootstrap only: seed ghost orders from the first message and its order-book row. Must be called
  // before the first apply(). Returns false (and does nothing) if bootstrap is off or messages were
  // already applied. Allocates.
  bool seed_ghosts(const Message& first, const BookRow& first_row);

  // Apply one message; returns the anomalies found (0 if none). Does nothing once stopped().
  AnomalyMask apply(const Message& m, std::uint64_t index, std::uint64_t line);

  bool stopped() const noexcept { return stopped_; }
  const OrderBook& book() const noexcept { return book_; }
  const ReplayCounters& counters() const noexcept { return counters_; }
  const GhostCounters& ghosts() const noexcept { return ghosts_; }
  const FirstAnomaly& first_anomaly() const noexcept { return first_; }
  const Config& config() const noexcept { return cfg_; }

 private:
  struct Ghost {
    OrderId id;
    Side side;
    Price price;
  };

  AnomalyMask check_and_apply(const Message& m);
  AnomalyMask apply_to_ghosts(const Message& m, bool& resolved);
  void record(Anomaly a, AnomalyMask& mask) noexcept;

  Config cfg_;
  OrderBook book_;
  std::vector<Fill> fills_;
  std::vector<Ghost> ghost_list_;  // creation order = time priority
  ReplayCounters counters_;
  GhostCounters ghosts_;
  FirstAnomaly first_;
  Time last_time_ = 0;
  bool have_time_ = false;
  bool stopped_ = false;
};

}  // namespace lob::lobster
