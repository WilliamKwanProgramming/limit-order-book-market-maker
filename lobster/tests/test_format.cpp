// Parser and reader tests: hand-picked edge cases plus randomized comparisons against deliberately
// naive reference parsers written with std::string operations.
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "lob/check.hpp"
#include "lob/lobster_format.hpp"
#include "lob/lobster_reader.hpp"
#include "lob/rng.hpp"

using namespace lob;
using namespace lob::lobster;

namespace {

Time t_of(const char* s) {
  Time t = -1;
  const ParseError e = parse_time(s, t);
  return e.ok() ? t : -1;
}

ParseCode time_code(const char* s) {
  Time t = 0;
  return parse_time(s, t).code;
}

ParseError msg(const std::string& line, Message& m) { return parse_message(line, m); }

std::vector<std::string> read_lines(const std::string& data, std::size_t buffer) {
  StringSource src(data);
  LineReader r(src, buffer);
  std::vector<std::string> out;
  std::string_view line;
  while (r.next(line)) out.emplace_back(line);
  return out;
}

// ---- Naive references ---------------------------------------------------------------------------

std::string ref_trim(const std::string& s) {
  std::size_t b = s.find_first_not_of(" \t");
  if (b == std::string::npos) return "";
  std::size_t e = s.find_last_not_of(" \t");
  return s.substr(b, e - b + 1);
}

bool all_digits(const std::string& s) {
  if (s.empty()) return false;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
  }
  return true;
}

// Returns the value, or nullopt for any error.
std::optional<std::int64_t> ref_int(const std::string& field) {
  std::string s = ref_trim(field);
  std::string digits = s;
  if (!s.empty() && s[0] == '-') digits = s.substr(1);
  if (!all_digits(digits)) return std::nullopt;
  try {
    return std::stoll(s);
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<std::int64_t> ref_time(const std::string& field) {
  std::string s = ref_trim(field);
  const std::size_t dot = s.find('.');
  std::string ip = s.substr(0, dot);
  std::string fp = dot == std::string::npos ? "0" : s.substr(dot + 1);
  if (!all_digits(ip) || !all_digits(fp) || fp.size() > 9) return std::nullopt;
  while (ip.size() > 1 && ip[0] == '0') ip.erase(0, 1);
  fp.append(9 - fp.size(), '0');
  // Seconds must satisfy seconds * 1e9 + 999999999 <= INT64_MAX, i.e. seconds <= 9223372035.
  if (ip.size() > 10 || (ip.size() == 10 && ip > "9223372035")) return std::nullopt;
  return std::stoll(ip) * 1000000000LL + std::stoll(fp);
}

std::string random_token(Rng& rng, const char* alphabet, std::size_t max_len) {
  const std::size_t n = static_cast<std::size_t>(rng.uniform_int(std::uint64_t{max_len + 1}));
  const std::size_t k = std::char_traits<char>::length(alphabet);
  std::string s;
  for (std::size_t i = 0; i < n; ++i) s += alphabet[rng.uniform_int(std::uint64_t{k})];
  return s;
}

}  // namespace

// ---- Time ---------------------------------------------------------------------------------------

LOB_TEST(time_fraction_digits) {
  LOB_CHECK_EQ(t_of("34200"), 34200 * kNanosPerSecond);
  LOB_CHECK_EQ(t_of("34200.1"), 34200 * kNanosPerSecond + 100000000);
  LOB_CHECK_EQ(t_of("34200.000000001"), 34200 * kNanosPerSecond + 1);
  LOB_CHECK_EQ(t_of("34200.123456789"), 34200 * kNanosPerSecond + 123456789);
  LOB_CHECK_EQ(t_of("34200.18960767"), 34200 * kNanosPerSecond + 189607670);
  LOB_CHECK_EQ(t_of("0"), 0);
  LOB_CHECK_EQ(t_of("0.0"), 0);
  LOB_CHECK_EQ(t_of("  57599.999999999\t"), 57599 * kNanosPerSecond + 999999999);
  LOB_CHECK_EQ(t_of("0034200.5"), 34200 * kNanosPerSecond + 500000000);
}

LOB_TEST(time_rejections) {
  LOB_CHECK(time_code("34200.1234567891") == ParseCode::TooManyFractionDigits);
  LOB_CHECK(time_code("-1.5") == ParseCode::NegativeTime);
  LOB_CHECK(time_code("-0") == ParseCode::NegativeTime);
  LOB_CHECK(time_code("") == ParseCode::EmptyField);
  LOB_CHECK(time_code("   ") == ParseCode::EmptyField);
  LOB_CHECK(time_code("34200.") == ParseCode::BadTime);
  LOB_CHECK(time_code(".5") == ParseCode::BadTime);
  LOB_CHECK(time_code("3.42e4") == ParseCode::BadTime);
  LOB_CHECK(time_code("+34200") == ParseCode::BadTime);
  LOB_CHECK(time_code("34 200") == ParseCode::BadTime);
  LOB_CHECK(time_code("34200.1.2") == ParseCode::BadTime);
  LOB_CHECK(time_code("99999999999") == ParseCode::OutOfRange);
  // Largest accepted value and the first rejected one.
  LOB_CHECK_EQ(t_of("9223372035.999999999"), 9223372035999999999LL);
  LOB_CHECK(time_code("9223372036") == ParseCode::OutOfRange);
}

LOB_TEST(time_random_against_reference) {
  Rng rng(11);
  long compared = 0;
  long valid = 0;
  for (int i = 0; i < 200000; ++i) {
    std::string s;
    const auto shape = rng.uniform_int(std::uint64_t{4});
    if (shape == 0) {
      s = random_token(rng, "0123456789.-+e x", 14);
    } else {
      s = random_token(rng, "0123456789", 11);
      if (rng.bernoulli(0.8)) s += "." + random_token(rng, "0123456789", 11);
      if (rng.bernoulli(0.1)) s = " " + s + "\t";
      if (rng.bernoulli(0.05)) s = "-" + s;
    }
    Time got = 0;
    const ParseError e = parse_time(s, got);
    const auto want = ref_time(s);
    ++compared;
    if (want) ++valid;
    if (e.ok() != want.has_value() || (want && got != *want)) {
      LOB_CHECK(false);
      std::fprintf(stderr, "  time mismatch on \"%s\"\n", s.c_str());
      return;
    }
  }
  LOB_CHECK(valid > compared / 3);  // the generator produces plenty of valid times
}

// ---- Integers -----------------------------------------------------------------------------------

LOB_TEST(int_random_against_reference) {
  Rng rng(12);
  long valid = 0;
  for (int i = 0; i < 200000; ++i) {
    std::string s = random_token(rng, "0123456789", 20);
    if (rng.bernoulli(0.2)) s = "-" + s;
    if (rng.bernoulli(0.1)) s = " " + s + " ";
    if (rng.bernoulli(0.1)) s += random_token(rng, "+-.e x,", 2);
    std::int64_t got = 0;
    const ParseError e = parse_int(s, got);
    const auto want = ref_int(s);
    if (want) ++valid;
    if (e.ok() != want.has_value() || (want && got != *want)) {
      LOB_CHECK(false);
      std::fprintf(stderr, "  int mismatch on \"%s\"\n", s.c_str());
      return;
    }
  }
  LOB_CHECK(valid > 50000);
  std::int64_t v = 0;
  LOB_CHECK(parse_int("9223372036854775808", v).code == ParseCode::OutOfRange);
  LOB_CHECK(parse_int("1.0", v).code == ParseCode::NotAnInteger);
  LOB_CHECK(parse_int("+1", v).code == ParseCode::NotAnInteger);
  LOB_CHECK(parse_int("", v).code == ParseCode::EmptyField);
}

// ---- Message rows -------------------------------------------------------------------------------

LOB_TEST(message_basic_and_robustness) {
  Message m;
  LOB_CHECK(msg("34200.004241176,1,16113575,18,5853300,1", m).ok());
  LOB_CHECK_EQ(m.time, 34200 * kNanosPerSecond + 4241176);
  LOB_CHECK_EQ(m.type, 1);
  LOB_CHECK_EQ(m.id, 16113575);
  LOB_CHECK_EQ(m.size, 18);
  LOB_CHECK_EQ(m.price, 5853300);
  LOB_CHECK_EQ(m.direction, 1);
  LOB_CHECK(m.side() == Side::Bid);
  // Trailing comma, spaces, tabs.
  LOB_CHECK(msg(" 34200.5 , 3 ,7, 100 ,5853300 , -1 ,", m).ok());
  LOB_CHECK_EQ(m.type, 3);
  LOB_CHECK(m.side() == Side::Ask);
  LOB_CHECK(msg("34200,4,7,100,5853300,-1,  ", m).ok());
  LOB_CHECK_EQ(m.time, 34200 * kNanosPerSecond);
}

LOB_TEST(message_rejections) {
  Message m;
  LOB_CHECK(msg("34200,1,7,100,5853300", m).code == ParseCode::WrongFieldCount);
  LOB_CHECK(msg("34200,1,7,100,5853300,1,,", m).code == ParseCode::WrongFieldCount);
  LOB_CHECK(msg("34200,1,7,100,5853300,1,5", m).code == ParseCode::WrongFieldCount);
  LOB_CHECK(msg("", m).code == ParseCode::WrongFieldCount);
  ParseError e = msg("34200,1,7,1.5,5853300,1", m);
  LOB_CHECK(e.code == ParseCode::NotAnInteger);
  LOB_CHECK_EQ(e.field, 4);
  e = msg("34200,1,7,100,58533.00,1", m);
  LOB_CHECK(e.code == ParseCode::NotAnInteger);
  LOB_CHECK_EQ(e.field, 5);
  LOB_CHECK(msg("34200.1234567890,1,7,100,5853300,1", m).code == ParseCode::TooManyFractionDigits);
  LOB_CHECK(msg("-1,1,7,100,5853300,1", m).code == ParseCode::NegativeTime);
  LOB_CHECK(msg("34200,8,7,100,5853300,1", m).code == ParseCode::UnknownType);
  LOB_CHECK(msg("34200,0,7,100,5853300,1", m).code == ParseCode::UnknownType);
  LOB_CHECK(msg("34200,1,7,,5853300,1", m).code == ParseCode::EmptyField);
  // Per-type rules for types 1-4.
  for (int type = 1; type <= 4; ++type) {
    const std::string t = std::to_string(type);
    LOB_CHECK(msg("1," + t + ",0,100,5853300,1", m).code == ParseCode::InvalidId);
    LOB_CHECK(msg("1," + t + ",7,0,5853300,1", m).code == ParseCode::InvalidSize);
    LOB_CHECK(msg("1," + t + ",7,-5,5853300,1", m).code == ParseCode::InvalidSize);
    LOB_CHECK(msg("1," + t + ",7,100,0,1", m).code == ParseCode::InvalidPrice);
    LOB_CHECK(msg("1," + t + ",7,100,-1,1", m).code == ParseCode::InvalidPrice);
    LOB_CHECK(msg("1," + t + ",7,100,5853300,0", m).code == ParseCode::InvalidDirection);
    LOB_CHECK(msg("1," + t + ",7,100,5853300,2", m).code == ParseCode::InvalidDirection);
  }
}

LOB_TEST(message_types_5_6_7_are_lenient) {
  Message m;
  // Hidden execution and cross trade: any id, price and direction; size >= 0.
  LOB_CHECK(msg("34200,5,0,100,5853300,1", m).ok());
  LOB_CHECK(msg("34200,5,0,0,-1,0", m).ok());
  LOB_CHECK(msg("34200,6,-1,0,0,7", m).ok());
  LOB_CHECK(msg("34200,6,5,-1,5853300,1", m).code == ParseCode::InvalidSize);
  // Trading halt: unusual values everywhere.
  LOB_CHECK(msg("34200,7,0,0,-1,-1", m).ok());
  LOB_CHECK_EQ(m.price, -1);
  LOB_CHECK(msg("34200,7,0,0,1,0", m).ok());
  LOB_CHECK_EQ(m.direction, 0);
  LOB_CHECK(msg("34200,7,-3,-4,0,2", m).ok());
}

LOB_TEST(message_random_round_trip) {
  Rng rng(13);
  for (int i = 0; i < 50000; ++i) {
    Message want;
    want.type = static_cast<std::int32_t>(rng.uniform_int(1, 7));
    const std::int64_t sec = rng.uniform_int(0, 86399);
    const int digits = static_cast<int>(rng.uniform_int(0, 9));
    std::int64_t frac = 0;
    std::string fs;
    for (int d = 0; d < digits; ++d) {
      const auto c = rng.uniform_int(0, 9);
      fs += static_cast<char>('0' + c);
      frac = frac * 10 + c;
    }
    for (int d = digits; d < 9; ++d) frac *= 10;
    want.time = sec * kNanosPerSecond + frac;
    if (changes_visible_book(want.type)) {
      want.id = rng.uniform_int(1, 4000000000LL);
      want.size = rng.uniform_int(1, 100000);
      want.price = rng.uniform_int(1, 99999999999LL);
      want.direction = rng.bernoulli(0.5) ? 1 : -1;
    } else {
      want.id = rng.uniform_int(-5, 4000000000LL);
      want.size = want.type == 7 ? rng.uniform_int(-5, 5) : rng.uniform_int(0, 100000);
      want.price = rng.uniform_int(-2, 99999999);
      want.direction = rng.uniform_int(-2, 2);
    }
    const auto pad = [&]() { return std::string(static_cast<std::size_t>(rng.uniform_int(0, 1)), ' '); };
    std::string line = pad() + std::to_string(sec) + (digits ? "." + fs : "") + pad();
    for (std::int64_t v : {std::int64_t{want.type}, want.id, want.size, want.price, want.direction}) {
      line += "," + pad() + std::to_string(v) + pad();
    }
    if (rng.bernoulli(0.2)) line += ",";
    Message got;
    const ParseError e = parse_message(line, got);
    if (!e.ok() || got.time != want.time || got.type != want.type || got.id != want.id ||
        got.size != want.size || got.price != want.price || got.direction != want.direction) {
      LOB_CHECK(false);
      std::fprintf(stderr, "  round trip failed on \"%s\" (%s)\n", line.c_str(), to_string(e.code));
      return;
    }
  }
}

// ---- Order-book rows ----------------------------------------------------------------------------

LOB_TEST(book_row_levels_and_sentinels) {
  const std::string line =
      "5853400,200,5853300,18,9999999999,0,-9999999999,0";
  LOB_CHECK_EQ(count_book_levels(line), std::size_t{2});
  LOB_CHECK_EQ(count_book_levels(line + ","), std::size_t{2});
  LOB_CHECK_EQ(count_book_levels("1,2,3"), std::size_t{0});
  BookRow row;
  row.resize(2);
  LOB_CHECK(parse_book_row(line, row).ok());
  LOB_CHECK_EQ(row.asks[0].price, 5853400);
  LOB_CHECK_EQ(row.asks[0].qty, 200);
  LOB_CHECK_EQ(row.bids[0].price, 5853300);
  LOB_CHECK_EQ(row.bids[0].qty, 18);
  LOB_CHECK(row.asks[1].empty());
  LOB_CHECK_EQ(row.asks[1].price, 0);
  LOB_CHECK(row.bids[1].empty());
  LOB_CHECK(parse_book_row(" 5853400 ,200,5853300,18,9999999999,0,-9999999999,0 ,", row).ok());
  // Wrong field counts.
  LOB_CHECK(parse_book_row("5853400,200,5853300,18", row).code == ParseCode::WrongFieldCount);
  LOB_CHECK(parse_book_row(line + ",1", row).code == ParseCode::WrongFieldCount);
  LOB_CHECK(parse_book_row(line + ",,", row).code == ParseCode::WrongFieldCount);
  // Invalid levels.
  ParseError e = parse_book_row("9999999999,5,5853300,18,9999999999,0,-9999999999,0", row);
  LOB_CHECK(e.code == ParseCode::InvalidLevel);
  LOB_CHECK_EQ(e.field, 1);
  e = parse_book_row("5853400,200,-9999999999,3,9999999999,0,-9999999999,0", row);
  LOB_CHECK(e.code == ParseCode::InvalidLevel);
  LOB_CHECK_EQ(e.field, 3);
  LOB_CHECK(parse_book_row("5853400,0,5853300,18,9999999999,0,-9999999999,0", row).code ==
            ParseCode::InvalidLevel);
  LOB_CHECK(parse_book_row("5853400,200,5853300,18,-9999999999,0,-9999999999,0", row).code ==
            ParseCode::InvalidLevel);
  LOB_CHECK(parse_book_row("5853400,200,5853300,1.5,9999999999,0,-9999999999,0", row).code ==
            ParseCode::NotAnInteger);
}

// ---- Readers ------------------------------------------------------------------------------------

LOB_TEST(line_reader_terminators) {
  const auto lines = read_lines("a\r\nbb\n\nccc", 1 << 10);
  LOB_CHECK_EQ(lines.size(), std::size_t{4});
  LOB_CHECK_EQ(lines[0], std::string("a"));
  LOB_CHECK_EQ(lines[1], std::string("bb"));
  LOB_CHECK_EQ(lines[2], std::string(""));
  LOB_CHECK_EQ(lines[3], std::string("ccc"));
  LOB_CHECK_EQ(read_lines("x\n", 16).size(), std::size_t{1});
  LOB_CHECK_EQ(read_lines("", 16).size(), std::size_t{0});
  LOB_CHECK_EQ(read_lines("x\r", 16)[0], std::string("x"));
  LOB_CHECK_EQ(read_lines("\xEF\xBB\xBFx,y\n", 16)[0], std::string("x,y"));
}

LOB_TEST(line_reader_random_buffers) {
  Rng rng(14);
  for (int trial = 0; trial < 300; ++trial) {
    std::vector<std::string> want;
    std::string data;
    const int n = static_cast<int>(rng.uniform_int(0, 60));
    for (int i = 0; i < n; ++i) {
      std::string l = random_token(rng, "0123456789,.- ab", 90);  // can exceed the buffer
      want.push_back(l);
      data += l;
      if (i + 1 < n || rng.bernoulli(0.5)) data += rng.bernoulli(0.3) ? "\r\n" : "\n";
    }
    const auto buffer = static_cast<std::size_t>(rng.uniform_int(16, 70));
    const auto got = read_lines(data, buffer);
    if (got != want) {
      LOB_CHECK(false);
      std::fprintf(stderr, "  line reader mismatch, trial %d, buffer %zu\n", trial, buffer);
      return;
    }
  }
}

LOB_TEST(message_reader_line_numbers_and_errors) {
  const std::string data =
      "34200.1,1,1,100,1000000,1\r\n"
      "\r\n"
      "34200.2,1,2,100,1000100,-1\r\n"
      "34200.3,9,3,100,1000100,-1\r\n"
      "34200.4,3,1,100,1000000,1";  // no final newline
  StringSource src(data);
  MessageReader r(src, 16);
  Message m;
  LOB_CHECK(r.next(m) == ReadResult::Record);
  LOB_CHECK_EQ(r.line(), std::uint64_t{1});
  LOB_CHECK(r.next(m) == ReadResult::Record);
  LOB_CHECK_EQ(r.line(), std::uint64_t{3});
  LOB_CHECK_EQ(m.id, 2);
  LOB_CHECK(r.next(m) == ReadResult::Malformed);
  LOB_CHECK_EQ(r.line(), std::uint64_t{4});
  LOB_CHECK(r.error().code == ParseCode::UnknownType);
  LOB_CHECK_EQ(std::string(r.raw_line()), std::string("34200.3,9,3,100,1000100,-1"));
  LOB_CHECK(r.next(m) == ReadResult::Record);
  LOB_CHECK_EQ(r.line(), std::uint64_t{5});
  LOB_CHECK_EQ(m.type, 3);
  LOB_CHECK(r.next(m) == ReadResult::End);
  LOB_CHECK_EQ(r.blank_lines(), std::uint64_t{1});
  LOB_CHECK(!r.io_failed());
}

LOB_TEST(book_reader_infers_and_enforces_levels) {
  const std::string data =
      "1000100,5,1000000,7\n"
      "1000100,5,1000000,7,1000200,1,999900,2\n"
      "9999999999,0,-9999999999,0\n";
  StringSource src(data);
  BookReader r(src);
  BookRow row;
  LOB_CHECK(r.next(row) == ReadResult::Record);
  LOB_CHECK_EQ(r.levels(), std::size_t{1});
  LOB_CHECK(r.next(row) == ReadResult::Malformed);
  LOB_CHECK(r.error().code == ParseCode::LevelCountMismatch);
  LOB_CHECK(r.next(row) == ReadResult::Record);
  LOB_CHECK(row.asks[0].empty() && row.bids[0].empty());
  LOB_CHECK(r.next(row) == ReadResult::End);

  StringSource src2(data);
  BookReader r2(src2, 2);  // --levels 2: first row has the wrong count
  LOB_CHECK(r2.next(row) == ReadResult::Malformed);
  LOB_CHECK(r2.error().code == ParseCode::LevelCountMismatch);
  LOB_CHECK(r2.next(row) == ReadResult::Record);

  StringSource src3("1,2,3\n");
  BookReader r3(src3);
  LOB_CHECK(r3.next(row) == ReadResult::Malformed);
  LOB_CHECK(r3.error().code == ParseCode::InvalidLevelCount);
}

LOB_TEST(stream_source_reads_istream) {
  std::istringstream in("34200,1,1,100,1000000,1\n34201,3,1,100,1000000,1\n");
  StreamSource src(in);
  MessageReader r(src);
  Message m;
  int n = 0;
  while (r.next(m) == ReadResult::Record) ++n;
  LOB_CHECK_EQ(n, 2);
  LOB_CHECK(!r.io_failed());
}

LOB_TEST_MAIN()
