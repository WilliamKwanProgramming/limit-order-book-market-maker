// LOBSTER file format: record types and row parsers.
//
// Message row:    time,type,order_id,size,price,direction            (no header)
// Order-book row: ask_px_1,ask_sz_1,bid_px_1,bid_sz_1, ... level N   (no header, 4*N fields)
//
// The layout follows the public LOBSTER documentation as summarised in docs/design.md ("LOBSTER
// format notes"). None of it has been checked against a real file in this repository.
//
// PARSING RULES (every parser here is noexcept and never allocates).
//   Fields are separated by ','. Spaces and tabs around a field are ignored. One trailing empty
//   field (a trailing comma) is tolerated. Line terminators ("\n" or "\r\n") are removed by the
//   reader before a row gets here.
//   Integers: optional '-', decimal digits, nothing else (no '+', no exponent, no decimal point).
//   Time: "S" or "S.F" with S one or more digits and F 1 to 9 digits, parsed to integer
//     nanoseconds without floating point ("34200.1" = 34200100000000 ns). A leading '-' is
//     rejected as a negative time; "34200." (empty fraction) is rejected.
//   Message rows, per type:
//     types 1-4: order_id > 0, size > 0, price > 0, direction +1 or -1;
//     types 5, 6: size >= 0; order_id, price and direction are not restricted;
//     type 7 (trading halt): no restriction on order_id, size, price or direction;
//     any other type is rejected.
//   Order-book rows: every level is either (price > 0, size > 0) or the side's empty-level
//     sentinel (ask 9999999999, bid -9999999999) with size 0. Empty levels are stored as
//     {price 0, qty 0}.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "lob/types.hpp"

namespace lob::lobster {

inline constexpr Price kEmptyAskPrice = 9999999999;
inline constexpr Price kEmptyBidPrice = -9999999999;
// Upper bound on the level count accepted in an order-book file (LOBSTER offers far fewer).
inline constexpr std::size_t kMaxBookLevels = 1000;

// One message row. Fields are kept as parsed; side() and order_id() are meaningful only for
// types 1-4, whose values the parser has validated.
struct Message {
  Time time = 0;              // nanoseconds after midnight
  std::int32_t type = 0;      // 1..7
  std::int64_t id = 0;        // order id as written
  Qty size = 0;
  Price price = 0;            // LOBSTER ticks (USD x 10000), used as-is as lob::Price
  std::int64_t direction = 0; // +1 buy limit order, -1 sell limit order (types 1-4)

  Side side() const noexcept { return direction > 0 ? Side::Bid : Side::Ask; }
  OrderId order_id() const noexcept { return static_cast<OrderId>(id); }
};

// True for the message types that change the visible book (1 to 4).
constexpr bool changes_visible_book(std::int32_t type) noexcept { return type >= 1 && type <= 4; }

enum class ParseCode : std::uint8_t {
  Ok = 0,
  WrongFieldCount,
  EmptyField,
  NotAnInteger,
  OutOfRange,
  BadTime,
  TooManyFractionDigits,
  NegativeTime,
  UnknownType,
  InvalidId,
  InvalidSize,
  InvalidPrice,
  InvalidDirection,
  InvalidLevelCount,   // order-book row: field count is not a positive multiple of 4
  LevelCountMismatch,  // order-book row: level count differs from the first row or --levels
  InvalidLevel,        // order-book row: not (price > 0, size > 0) and not a sentinel with size 0
};

const char* to_string(ParseCode c) noexcept;

struct ParseError {
  ParseCode code = ParseCode::Ok;
  std::int32_t field = 0;  // 1-based field index, 0 if the row as a whole is wrong
  bool ok() const noexcept { return code == ParseCode::Ok; }
};

// Single fields (surrounding spaces and tabs allowed).
ParseError parse_int(std::string_view field, std::int64_t& out) noexcept;
ParseError parse_time(std::string_view field, Time& out) noexcept;

// A message row. On error `out` is unspecified.
ParseError parse_message(std::string_view line, Message& out) noexcept;

// One level of an order-book row. qty == 0 means the level is empty (sentinel).
struct BookLevel {
  Price price = 0;
  Qty qty = 0;
  bool empty() const noexcept { return qty == 0; }
  friend bool operator==(const BookLevel& a, const BookLevel& b) noexcept {
    return a.price == b.price && a.qty == b.qty;
  }
  friend bool operator!=(const BookLevel& a, const BookLevel& b) noexcept { return !(a == b); }
};

// One order-book row: level i of each side at index i (best first).
struct BookRow {
  std::vector<BookLevel> asks;
  std::vector<BookLevel> bids;

  std::size_t levels() const noexcept { return asks.size(); }
  void resize(std::size_t n) {
    asks.resize(n);
    bids.resize(n);
  }
  friend bool operator==(const BookRow& a, const BookRow& b) noexcept {
    return a.asks == b.asks && a.bids == b.bids;
  }
};

// Number of levels in an order-book row (field count / 4, ignoring one trailing empty field), or
// 0 if the field count is not a positive multiple of 4.
std::size_t count_book_levels(std::string_view line) noexcept;

// Parse an order-book row with exactly out.levels() levels per side (resize `out` first).
ParseError parse_book_row(std::string_view line, BookRow& out) noexcept;

// True if the line holds only spaces and tabs (or nothing).
bool is_blank(std::string_view line) noexcept;

}  // namespace lob::lobster
