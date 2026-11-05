#include "lob/lobster_format.hpp"

#include <charconv>
#include <cstdint>
#include <limits>
#include <system_error>

namespace lob::lobster {

namespace {

constexpr bool is_space(char c) noexcept { return c == ' ' || c == '\t'; }
constexpr bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

std::string_view trim(std::string_view s) noexcept {
  std::size_t b = 0;
  std::size_t e = s.size();
  while (b < e && is_space(s[b])) ++b;
  while (e > b && is_space(s[e - 1])) --e;
  return s.substr(b, e - b);
}

// Splits `line` at commas into at most `max_fields` views. Returns the number of fields found, or
// max_fields + 1 if there are more. A trailing blank field after `expected` fields is dropped.
std::size_t split(std::string_view line, std::string_view* fields, std::size_t max_fields,
                  std::size_t expected) noexcept {
  std::size_t n = 0;
  std::size_t start = 0;
  for (;;) {
    const std::size_t comma = line.find(',', start);
    const std::size_t len =
        comma == std::string_view::npos ? std::string_view::npos : comma - start;
    const std::string_view f = line.substr(start, len);
    if (n == max_fields) return max_fields + 1;
    fields[n++] = f;
    if (comma == std::string_view::npos) break;
    start = comma + 1;
  }
  if (n == expected + 1 && is_blank(fields[n - 1])) --n;
  return n;
}

constexpr std::int64_t kNanos = 1'000'000'000;
// Largest whole-second count whose nanosecond value (plus any fraction) fits in int64.
constexpr std::int64_t kMaxSeconds =
    (std::numeric_limits<std::int64_t>::max() - (kNanos - 1)) / kNanos;

ParseError at(ParseCode c, std::int32_t field) noexcept { return ParseError{c, field}; }

ParseError with_field(ParseError e, std::int32_t field) noexcept {
  if (!e.ok()) e.field = field;
  return e;
}

}  // namespace

const char* to_string(ParseCode c) noexcept {
  switch (c) {
    case ParseCode::Ok: return "ok";
    case ParseCode::WrongFieldCount: return "wrong number of fields";
    case ParseCode::EmptyField: return "empty field";
    case ParseCode::NotAnInteger: return "not an integer";
    case ParseCode::OutOfRange: return "integer out of range";
    case ParseCode::BadTime: return "malformed time";
    case ParseCode::TooManyFractionDigits: return "more than 9 fractional digits in time";
    case ParseCode::NegativeTime: return "negative time";
    case ParseCode::UnknownType: return "unknown message type (expected 1-7)";
    case ParseCode::InvalidId: return "order id must be > 0 for types 1-4";
    case ParseCode::InvalidSize: return "invalid size for this message type";
    case ParseCode::InvalidPrice: return "price must be > 0 for types 1-4";
    case ParseCode::InvalidDirection: return "direction must be +1 or -1 for types 1-4";
    case ParseCode::InvalidLevelCount: return "field count is not a positive multiple of 4";
    case ParseCode::LevelCountMismatch: return "level count differs from the expected one";
    case ParseCode::InvalidLevel:
      return "level is neither (price > 0, size > 0) nor the empty-level sentinel with size 0";
  }
  return "unknown";
}

bool is_blank(std::string_view line) noexcept {
  for (char c : line) {
    if (!is_space(c)) return false;
  }
  return true;
}

ParseError parse_int(std::string_view field, std::int64_t& out) noexcept {
  const std::string_view s = trim(field);
  if (s.empty()) return at(ParseCode::EmptyField, 0);
  // from_chars accepts a leading '-' and digits only; it rejects '+', spaces and exponents.
  const char* first = s.data();
  const char* last = s.data() + s.size();
  const auto r = std::from_chars(first, last, out);
  if (r.ec == std::errc::result_out_of_range) return at(ParseCode::OutOfRange, 0);
  if (r.ec != std::errc() || r.ptr != last) return at(ParseCode::NotAnInteger, 0);
  return {};
}

ParseError parse_time(std::string_view field, Time& out) noexcept {
  const std::string_view s = trim(field);
  if (s.empty()) return at(ParseCode::EmptyField, 0);
  if (s[0] == '-') return at(ParseCode::NegativeTime, 0);
  std::size_t i = 0;
  std::int64_t seconds = 0;
  while (i < s.size() && is_digit(s[i])) {
    const int d = s[i] - '0';
    if (seconds > (kMaxSeconds - d) / 10) return at(ParseCode::OutOfRange, 0);
    seconds = seconds * 10 + d;
    ++i;
  }
  if (i == 0) return at(ParseCode::BadTime, 0);
  std::int64_t frac = 0;
  if (i < s.size()) {
    if (s[i] != '.') return at(ParseCode::BadTime, 0);
    ++i;
    const std::size_t frac_begin = i;
    while (i < s.size() && is_digit(s[i])) ++i;
    if (i != s.size() || i == frac_begin) return at(ParseCode::BadTime, 0);
    const std::size_t digits = i - frac_begin;
    if (digits > 9) return at(ParseCode::TooManyFractionDigits, 0);
    for (std::size_t k = frac_begin; k < i; ++k) frac = frac * 10 + (s[k] - '0');
    for (std::size_t k = digits; k < 9; ++k) frac *= 10;
  }
  out = seconds * kNanos + frac;
  return {};
}

ParseError parse_message(std::string_view line, Message& out) noexcept {
  std::string_view f[7];  // room for one trailing blank field
  if (split(line, f, 7, 6) != 6) return at(ParseCode::WrongFieldCount, 0);

  ParseError e = with_field(parse_time(f[0], out.time), 1);
  if (!e.ok()) return e;
  std::int64_t type = 0;
  if (!(e = with_field(parse_int(f[1], type), 2)).ok()) return e;
  if (!(e = with_field(parse_int(f[2], out.id), 3)).ok()) return e;
  if (!(e = with_field(parse_int(f[3], out.size), 4)).ok()) return e;
  if (!(e = with_field(parse_int(f[4], out.price), 5)).ok()) return e;
  if (!(e = with_field(parse_int(f[5], out.direction), 6)).ok()) return e;

  if (type < 1 || type > 7) return at(ParseCode::UnknownType, 2);
  out.type = static_cast<std::int32_t>(type);
  if (changes_visible_book(out.type)) {
    if (out.id <= 0) return at(ParseCode::InvalidId, 3);
    if (out.size <= 0) return at(ParseCode::InvalidSize, 4);
    if (out.price <= 0) return at(ParseCode::InvalidPrice, 5);
    if (out.direction != 1 && out.direction != -1) return at(ParseCode::InvalidDirection, 6);
  } else if (out.type == 5 || out.type == 6) {
    if (out.size < 0) return at(ParseCode::InvalidSize, 4);
  }
  // Type 7: no field restrictions (halt rows use unusual values by design).
  return {};
}

std::size_t count_book_levels(std::string_view line) noexcept {
  std::size_t fields = 1;
  std::size_t last_comma = std::string_view::npos;
  for (std::size_t i = 0; i < line.size(); ++i) {
    if (line[i] == ',') {
      ++fields;
      last_comma = i;
    }
  }
  if (last_comma != std::string_view::npos && fields > 1 &&
      is_blank(line.substr(last_comma + 1))) {
    --fields;
  }
  if (fields % 4 != 0) return 0;
  return fields / 4;
}

ParseError parse_book_row(std::string_view line, BookRow& out) noexcept {
  const std::size_t n = out.levels();
  const std::size_t expected = 4 * n;
  std::size_t field = 0;
  std::size_t start = 0;
  bool done = false;
  while (!done) {
    std::size_t comma = line.find(',', start);
    if (comma == std::string_view::npos) {
      comma = line.size();
      done = true;
    }
    const std::string_view f = line.substr(start, comma - start);
    start = comma + 1;
    if (field >= expected) {
      // Only one trailing blank field is allowed after the last level.
      if (field == expected && done && is_blank(f)) break;
      return at(ParseCode::WrongFieldCount, 0);
    }
    std::int64_t v = 0;
    const ParseError e = with_field(parse_int(f, v), static_cast<std::int32_t>(field + 1));
    if (!e.ok()) return e;
    const std::size_t level = field / 4;
    switch (field % 4) {
      case 0: out.asks[level].price = v; break;
      case 1: out.asks[level].qty = v; break;
      case 2: out.bids[level].price = v; break;
      default: out.bids[level].qty = v; break;
    }
    ++field;
  }
  if (field != expected) return at(ParseCode::WrongFieldCount, 0);
  for (std::size_t i = 0; i < n; ++i) {
    BookLevel& a = out.asks[i];
    BookLevel& b = out.bids[i];
    const auto f1 = static_cast<std::int32_t>(4 * i + 1);
    if (a.price == kEmptyAskPrice && a.qty == 0) {
      a.price = 0;
    } else if (!(a.price > 0 && a.price != kEmptyAskPrice && a.qty > 0)) {
      return at(ParseCode::InvalidLevel, f1);
    }
    if (b.price == kEmptyBidPrice && b.qty == 0) {
      b.price = 0;
    } else if (!(b.price > 0 && b.qty > 0)) {
      return at(ParseCode::InvalidLevel, f1 + 2);
    }
  }
  return {};
}

}  // namespace lob::lobster
