// One pass over a message file (and optionally its order-book file): parse, replay, validate,
// collect statistics. Used by lobster_replay and by the tests.
//
// ALIGNMENT. Row k of the order-book file belongs to message k (blank lines are skipped in both
// files). A file that runs out before the other is a fatal error.
//
// MALFORMED ROWS. Strict policy: the first malformed message or order-book row stops the run.
// Lenient policy: it is counted and skipped; its pair is not validated.
//
// EXIT CODE (exit_code()):
//   2  fatal: unreadable input, no message rows, or the two files have different row counts
//   1  strict policy and any anomaly or malformed row; or any validation mismatch; or a type
//      5/6/7 row that differs from the previous row
//   0  otherwise. Under the lenient policy, anomalies and malformed rows are reported but do not
//      by themselves make the exit code non-zero.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

#include "lob/lobster_reader.hpp"
#include "lob/lobster_replayer.hpp"
#include "lob/lobster_stats.hpp"
#include "lob/lobster_validator.hpp"

namespace lob::lobster {

// How much work a pass does (lobster_replay --bench times each one separately).
enum class Stage : std::uint8_t {
  Parse,     // parse the message file only
  Replay,    // parse the message file and replay it (with bootstrap, also the first book row)
  Validate,  // parse both files, replay, validate every row (the default)
};

struct SessionOptions {
  Policy policy = Policy::Strict;
  bool bootstrap = false;
  bool stats = false;
  std::size_t levels = 0;  // 0 = infer from the first order-book row
  Stage stage = Stage::Validate;
};

struct LineError {
  bool set = false;
  std::uint64_t index = 0;  // 1-based record number
  std::uint64_t line = 0;
  ParseError error;
  std::string text;  // the offending line (truncated)
};

struct SessionResult {
  SessionOptions options;
  bool has_book = false;
  std::size_t levels = 0;
  std::uint64_t message_records = 0;  // non-blank message rows read (well-formed or not)
  std::uint64_t message_parse_errors = 0;
  std::uint64_t book_parse_errors = 0;
  std::uint64_t message_blank_lines = 0;
  std::uint64_t book_blank_lines = 0;
  LineError first_message_error;
  LineError first_book_error;
  bool stopped_early = false;
  std::string stop_reason;
  std::string fatal;
  ReplayCounters replay;
  GhostCounters ghosts;
  FirstAnomaly first_anomaly;
  bool validated = false;
  ValidationReport validation;
  bool has_stats = false;
  StatsResult stats;
  std::size_t final_orders = 0;  // orders resting at the end (ghosts included)

  int exit_code() const noexcept;
};

// `book` may be null (no validation, no bootstrap).
SessionResult run_session(ByteSource& messages, ByteSource* book, const SessionOptions& opt);

// Human-readable summary.
void print_summary(std::FILE* out, const SessionResult& r);
// Machine-readable summary: one key=value per line.
void print_key_values(std::FILE* out, const SessionResult& r);

}  // namespace lob::lobster
