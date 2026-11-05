// Compares the engine's book with the LOBSTER order-book file after every message.
//
// After message k, level i (i < N) of each side of the engine (OrderBook::depth) must equal level
// i of row k; a level the engine does not have must be empty (sentinel) in the row. A row with a
// difference counts once in `mismatched`, and once in either `price_structure` (some level's price
// or emptiness differs) or `size_only` (all prices agree, some size differs). Per-level counters
// count each differing level index on each side.
//
// Rows for message types 5, 6 and 7 must also equal the previous row (those messages do not change
// the visible book); violations are counted separately in `unchanged_violations`.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "lob/lobster_format.hpp"
#include "lob/order_book.hpp"

namespace lob::lobster {

struct MismatchDetail {
  bool set = false;
  std::uint64_t index = 0;  // 1-based message number
  std::uint64_t line = 0;   // message file line
  Message message;
  BookRow expected;  // from the order-book file
  BookRow actual;    // from the engine
};

struct ValidationReport {
  std::size_t levels = 0;
  std::uint64_t checked = 0;          // rows compared with the engine
  std::uint64_t mismatched = 0;       // rows with any difference
  std::uint64_t price_structure = 0;  // ... of which some price or emptiness differs
  std::uint64_t size_only = 0;        // ... of which only sizes differ
  std::vector<std::uint64_t> ask_level_mismatches;  // index i = level i + 1
  std::vector<std::uint64_t> bid_level_mismatches;
  std::uint64_t unchanged_checked = 0;     // type 5/6/7 rows compared with the previous row
  std::uint64_t unchanged_violations = 0;  // ... that differed
  std::uint64_t first_unchanged_violation_index = 0;
  std::uint64_t skipped = 0;  // rows not compared (malformed message or row)
  MismatchDetail first;
};

class Validator {
 public:
  explicit Validator(std::size_t levels);

  // Compare the engine after message `m` with `row`. Returns true if they agree.
  bool check(const OrderBook& book, const Message& m, std::uint64_t index, std::uint64_t line,
             const BookRow& row);
  // A message/row pair that could not be compared. `row` (may be null) becomes the previous row
  // for the type 5/6/7 check.
  void skip(const BookRow* row);

  const ValidationReport& report() const noexcept { return report_; }

 private:
  void engine_row(const OrderBook& book, BookRow& out);

  ValidationReport report_;
  std::vector<Level> depth_;
  BookRow actual_;
  BookRow prev_;
  bool have_prev_ = false;
};

}  // namespace lob::lobster
