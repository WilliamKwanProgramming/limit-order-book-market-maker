#include "lob/lobster_session.hpp"

#include <cinttypes>
#include <optional>

namespace lob::lobster {

namespace {

constexpr std::size_t kMaxQuotedLine = 200;

void note(LineError& e, std::uint64_t index, std::uint64_t line, const ParseError& err,
          std::string_view text) {
  if (e.set) return;
  e.set = true;
  e.index = index;
  e.line = line;
  e.error = err;
  e.text.assign(text.substr(0, kMaxQuotedLine));
}

const char* stage_name(Stage s) {
  switch (s) {
    case Stage::Parse: return "parse";
    case Stage::Replay: return "replay";
    case Stage::Validate: return "validate";
  }
  return "?";
}

}  // namespace

int SessionResult::exit_code() const noexcept {
  if (!fatal.empty()) return 2;
  if (options.policy == Policy::Strict &&
      (replay.total_anomalies() > 0 || message_parse_errors > 0 || book_parse_errors > 0)) {
    return 1;
  }
  if (validated && (validation.mismatched > 0 || validation.unchanged_violations > 0)) return 1;
  return 0;
}

SessionResult run_session(ByteSource& messages, ByteSource* book, const SessionOptions& opt) {
  SessionResult r;
  r.options = opt;
  const bool read_book = book != nullptr && (opt.stage == Stage::Validate ||
                                             (opt.stage == Stage::Replay && opt.bootstrap));
  r.has_book = book != nullptr;
  r.has_stats = opt.stats && opt.stage != Stage::Parse;

  MessageReader mr(messages);
  std::optional<BookReader> br;
  if (read_book) br.emplace(*book, opt.levels);
  Replayer rep(Replayer::Config{opt.policy, opt.bootstrap});
  std::optional<Validator> val;
  Stats stats;

  Message m;
  BookRow row;
  std::uint64_t index = 0;
  for (;;) {
    const ReadResult mres = mr.next(m);
    if (mres == ReadResult::End) break;
    ++index;
    ++r.message_records;

    // The order-book row that belongs to this message.
    bool row_ok = false;
    const bool want_row = br && (opt.stage == Stage::Validate || index == 1);
    if (want_row) {
      const ReadResult bres = br->next(row);
      if (bres == ReadResult::End) {
        r.fatal = "order-book file has fewer rows than the message file (no row for message " +
                  std::to_string(index) + ")";
        break;
      }
      if (bres == ReadResult::Malformed) {
        ++r.book_parse_errors;
        note(r.first_book_error, index, br->line(), br->error(), br->raw_line());
        if (opt.policy == Policy::Strict) {
          r.stopped_early = true;
          r.stop_reason = "malformed order-book row";
          break;
        }
      } else {
        row_ok = true;
      }
      if (!val && br->levels() > 0 && opt.stage == Stage::Validate) val.emplace(br->levels());
    }

    if (mres == ReadResult::Malformed) {
      ++r.message_parse_errors;
      note(r.first_message_error, index, mr.line(), mr.error(), mr.raw_line());
      if (opt.policy == Policy::Strict) {
        r.stopped_early = true;
        r.stop_reason = "malformed message row";
        break;
      }
      if (val) val->skip(row_ok ? &row : nullptr);
      continue;
    }
    if (opt.stage == Stage::Parse) continue;

    if (index == 1) {
      if (opt.bootstrap && row_ok) rep.seed_ghosts(m, row);
      if (r.has_stats) stats.start(rep.book());
    }
    rep.apply(m, index, mr.line());
    if (rep.stopped()) {
      r.stopped_early = true;
      r.stop_reason = "anomaly (strict policy)";
      break;
    }
    if (val) {
      if (row_ok) {
        val->check(rep.book(), m, index, mr.line(), row);
      } else {
        val->skip(nullptr);
      }
    }
    if (r.has_stats) stats.observe(m, rep.book());
  }

  if (mr.io_failed()) r.fatal = "read error in the message file";
  if (br && br->io_failed()) r.fatal = "read error in the order-book file";
  if (r.fatal.empty() && r.message_records == 0) r.fatal = "the message file has no rows";
  if (r.fatal.empty() && !r.stopped_early && br && opt.stage == Stage::Validate) {
    if (br->next(row) != ReadResult::End) {
      r.fatal = "order-book file has more rows than the message file";
    }
  }
  r.message_blank_lines = mr.blank_lines();
  if (br) {
    r.book_blank_lines = br->blank_lines();
    r.levels = br->levels();
  }
  r.replay = rep.counters();
  r.ghosts = rep.ghosts();
  r.first_anomaly = rep.first_anomaly();
  if (val) {
    r.validated = true;
    r.validation = val->report();
  }
  if (r.has_stats) r.stats = stats.result();
  r.final_orders = rep.book().order_count();
  return r;
}

// ---- Output -------------------------------------------------------------------------------------

namespace {

void print_message(std::FILE* out, const Message& m) {
  std::fprintf(out,
               "time_ns=%" PRId64 " type=%" PRId32 " id=%" PRId64 " size=%" PRId64
               " price=%" PRId64 " direction=%" PRId64,
               m.time, m.type, m.id, m.size, m.price, m.direction);
}

void print_level(std::FILE* out, const BookLevel& l) {
  if (l.empty()) {
    std::fprintf(out, " %22s", "empty");
  } else {
    std::fprintf(out, " %12" PRId64 " x %7" PRId64, l.price, l.qty);
  }
}

void print_anomaly_names(std::FILE* out, AnomalyMask mask) {
  bool first = true;
  for (std::size_t k = 0; k < kAnomalyKinds; ++k) {
    if (mask & bit(static_cast<Anomaly>(k))) {
      std::fprintf(out, "%s%s", first ? "" : ",", to_string(static_cast<Anomaly>(k)));
      first = false;
    }
  }
}

}  // namespace

void print_summary(std::FILE* out, const SessionResult& r) {
  const ReplayCounters& c = r.replay;
  std::fprintf(out, "policy: %s   bootstrap: %s   stage: %s\n", to_string(r.options.policy),
               r.options.bootstrap ? "on" : "off", stage_name(r.options.stage));
  std::fprintf(out, "message rows: %" PRIu64 " (malformed %" PRIu64 ", blank lines skipped %" PRIu64
               ")\n", r.message_records, r.message_parse_errors, r.message_blank_lines);
  if (r.first_message_error.set) {
    std::fprintf(out,
                 "  first malformed message: line %" PRIu64 ", field %" PRId32 ": %s: \"%s\"\n",
                 r.first_message_error.line, r.first_message_error.error.field,
                 to_string(r.first_message_error.error.code), r.first_message_error.text.c_str());
  }
  if (r.has_book) {
    std::fprintf(out, "order-book levels: %zu   malformed rows: %" PRIu64 "\n", r.levels,
                 r.book_parse_errors);
    if (r.first_book_error.set) {
      std::fprintf(out, "  first malformed order-book row: line %" PRIu64 ", field %" PRId32
                   ": %s: \"%s\"\n", r.first_book_error.line, r.first_book_error.error.field,
                   to_string(r.first_book_error.error.code), r.first_book_error.text.c_str());
    }
  }
  std::fprintf(out, "messages replayed: %" PRIu64 "   by type:", c.messages);
  for (int t = 1; t <= 7; ++t) {
    std::fprintf(out, " %d=%" PRIu64, t, c.by_type[static_cast<std::size_t>(t)]);
  }
  std::fprintf(out, "\nanomalies: %" PRIu64 " in %" PRIu64 " messages\n", c.total_anomalies(),
               c.messages_with_anomaly);
  for (std::size_t k = 0; k < kAnomalyKinds; ++k) {
    std::fprintf(out, "  %-30s %" PRIu64 "\n", to_string(static_cast<Anomaly>(k)), c.anomalies[k]);
  }
  std::fprintf(out, "  engine calls skipped (lenient): %" PRIu64 "   engine rejections: %" PRIu64
               "\n", c.skipped_engine_calls, c.engine_rejections);
  if (r.first_anomaly.set) {
    std::fprintf(out, "  first anomaly: message %" PRIu64 " (line %" PRIu64 "): ",
                 r.first_anomaly.index, r.first_anomaly.line);
    print_anomaly_names(out, r.first_anomaly.mask);
    std::fprintf(out, "\n    ");
    print_message(out, r.first_anomaly.message);
    std::fprintf(out, "\n");
  }
  if (r.ghosts.enabled) {
    const GhostCounters& g = r.ghosts;
    std::fprintf(out, "bootstrap (HEURISTIC, untested on real LOBSTER data): seeded=%s"
                 " first_message_undone=%s\n", g.seeded ? "yes" : "no",
                 g.first_message_undone ? "yes" : "no");
    std::fprintf(out, "  ghosts created %" PRIu64 " (size %" PRId64 "), references resolved %"
                 PRIu64 " (size %" PRId64 "), ghosts emptied %" PRIu64
                 ", unexplained references %" PRIu64 ", shortfalls %" PRIu64 "\n",
                 g.created, g.created_qty, g.references_resolved, g.consumed_qty, g.emptied,
                 g.unexplained, g.shortfall);
  }
  if (r.validated) {
    const ValidationReport& v = r.validation;
    std::fprintf(out, "validation: %" PRIu64 " rows checked, %" PRIu64 " with a mismatch (price"
                 " structure %" PRIu64 ", size only %" PRIu64 "), %" PRIu64 " not checked\n",
                 v.checked, v.mismatched, v.price_structure, v.size_only, v.skipped);
    std::fprintf(out, "  type 5/6/7 rows equal to the previous row: %" PRIu64 " of %" PRIu64 "\n",
                 v.unchanged_checked - v.unchanged_violations, v.unchanged_checked);
    if (v.mismatched > 0) {
      std::fprintf(out, "  mismatches per level (ask/bid):");
      for (std::size_t i = 0; i < v.levels; ++i) {
        std::fprintf(out, " L%zu=%" PRIu64 "/%" PRIu64, i + 1, v.ask_level_mismatches[i],
                     v.bid_level_mismatches[i]);
      }
      std::fprintf(out, "\n  last mismatching row: message %" PRIu64 "\n",
                   v.last_mismatch_index);
    }
    if (v.first.set) {
      std::fprintf(out, "  first mismatch: message %" PRIu64 " (line %" PRIu64 "): ", v.first.index,
                   v.first.line);
      print_message(out, v.first.message);
      std::fprintf(out, "\n    %5s %22s %22s %22s %22s\n", "level", "ask (file)", "ask (engine)",
                   "bid (file)", "bid (engine)");
      for (std::size_t i = 0; i < v.levels; ++i) {
        std::fprintf(out, "    %5zu", i + 1);
        print_level(out, v.first.expected.asks[i]);
        print_level(out, v.first.actual.asks[i]);
        print_level(out, v.first.expected.bids[i]);
        print_level(out, v.first.actual.bids[i]);
        std::fprintf(out, "%s\n", (v.first.expected.asks[i] != v.first.actual.asks[i] ||
                                   v.first.expected.bids[i] != v.first.actual.bids[i])
                                      ? "   <--"
                                      : "");
      }
    }
  }
  if (r.has_stats) {
    const StatsResult& s = r.stats;
    std::fprintf(out, "stats: executed (type 4) against bids %" PRId64 ", against asks %" PRId64
                 "\n  hidden (type 5) by direction as written in the file (not observable per"
                 " the public course notes): +1 %" PRId64 ", -1 %" PRId64 ", other %" PRId64 "\n",
                 s.executed_bid, s.executed_ask, s.hidden_buy, s.hidden_sell, s.hidden_other);
    std::fprintf(out, "  time-weighted mean spread %.4f ticks over %.3f s two-sided; mean best bid"
                 " size %.2f, best ask size %.2f\n", s.mean_spread_ticks, s.two_sided_seconds,
                 s.mean_best_bid_qty, s.mean_best_ask_qty);
    std::fprintf(out, "  messages changing the best price: %.4f; best price or size: %.4f\n",
                 s.frac_best_price_changes(), s.frac_best_quote_changes());
  }
  std::fprintf(out, "orders resting at the end: %zu\n", r.final_orders);
  if (r.stopped_early) std::fprintf(out, "STOPPED EARLY: %s\n", r.stop_reason.c_str());
  if (!r.fatal.empty()) std::fprintf(out, "FATAL: %s\n", r.fatal.c_str());
  std::fprintf(out, "result: %s (exit code %d)\n", r.exit_code() == 0 ? "OK" : "FAILED",
               r.exit_code());
}

void print_key_values(std::FILE* out, const SessionResult& r) {
  const ReplayCounters& c = r.replay;
  std::fprintf(out, "policy=%s\nbootstrap=%d\nstage=%s\n", to_string(r.options.policy),
               r.options.bootstrap ? 1 : 0, stage_name(r.options.stage));
  std::fprintf(out, "exit_code=%d\nfatal=%s\nstopped_early=%d\nstop_reason=%s\n", r.exit_code(),
               r.fatal.c_str(), r.stopped_early ? 1 : 0, r.stop_reason.c_str());
  std::fprintf(out, "message_rows=%" PRIu64 "\nmessage_parse_errors=%" PRIu64
               "\nmessage_blank_lines=%" PRIu64 "\n", r.message_records, r.message_parse_errors,
               r.message_blank_lines);
  if (r.first_message_error.set) {
    std::fprintf(out, "first_message_error_line=%" PRIu64 "\nfirst_message_error=%s\n",
                 r.first_message_error.line, to_string(r.first_message_error.error.code));
  }
  std::fprintf(out, "has_book=%d\nlevels=%zu\nbook_parse_errors=%" PRIu64 "\n", r.has_book ? 1 : 0,
               r.levels, r.book_parse_errors);
  if (r.first_book_error.set) {
    std::fprintf(out, "first_book_error_line=%" PRIu64 "\nfirst_book_error=%s\n",
                 r.first_book_error.line, to_string(r.first_book_error.error.code));
  }
  std::fprintf(out, "messages=%" PRIu64 "\n", c.messages);
  for (int t = 1; t <= 7; ++t) {
    std::fprintf(out, "type%d=%" PRIu64 "\n", t, c.by_type[static_cast<std::size_t>(t)]);
  }
  std::fprintf(out, "anomalies=%" PRIu64 "\nmessages_with_anomaly=%" PRIu64 "\n",
               c.total_anomalies(), c.messages_with_anomaly);
  for (std::size_t k = 0; k < kAnomalyKinds; ++k) {
    std::fprintf(out, "anomaly_%s=%" PRIu64 "\n", to_string(static_cast<Anomaly>(k)),
                 c.anomalies[k]);
  }
  std::fprintf(out, "field_mismatches=%" PRIu64 "\nskipped_engine_calls=%" PRIu64
               "\nengine_rejections=%" PRIu64 "\n", c.field_mismatches(), c.skipped_engine_calls,
               c.engine_rejections);
  if (r.first_anomaly.set) {
    std::fprintf(out, "first_anomaly_index=%" PRIu64 "\nfirst_anomaly_line=%" PRIu64
                 "\nfirst_anomaly_kinds=", r.first_anomaly.index, r.first_anomaly.line);
    print_anomaly_names(out, r.first_anomaly.mask);
    std::fprintf(out, "\n");
  }
  if (r.ghosts.enabled) {
    const GhostCounters& g = r.ghosts;
    std::fprintf(out, "ghost_seeded=%d\nghost_first_message_undone=%d\nghosts_created=%" PRIu64
                 "\nghost_created_qty=%" PRId64 "\nghost_references_resolved=%" PRIu64
                 "\nghost_consumed_qty=%" PRId64 "\nghosts_emptied=%" PRIu64
                 "\nghost_unexplained=%" PRIu64 "\nghost_shortfall=%" PRIu64 "\n",
                 g.seeded ? 1 : 0, g.first_message_undone ? 1 : 0, g.created, g.created_qty,
                 g.references_resolved, g.consumed_qty, g.emptied, g.unexplained, g.shortfall);
  }
  std::fprintf(out, "validated=%d\n", r.validated ? 1 : 0);
  if (r.validated) {
    const ValidationReport& v = r.validation;
    std::fprintf(out, "rows_checked=%" PRIu64 "\nrows_mismatched=%" PRIu64
                 "\nmismatch_price_structure=%" PRIu64 "\nmismatch_size_only=%" PRIu64
                 "\nrows_not_checked=%" PRIu64 "\nunchanged_rows_checked=%" PRIu64
                 "\nunchanged_row_violations=%" PRIu64 "\n", v.checked, v.mismatched,
                 v.price_structure, v.size_only, v.skipped, v.unchanged_checked,
                 v.unchanged_violations);
    for (std::size_t i = 0; i < v.levels; ++i) {
      std::fprintf(out, "mismatch_ask_level%zu=%" PRIu64 "\nmismatch_bid_level%zu=%" PRIu64 "\n",
                   i + 1, v.ask_level_mismatches[i], i + 1, v.bid_level_mismatches[i]);
    }
    if (v.first.set) {
      std::fprintf(out, "first_mismatch_index=%" PRIu64 "\nfirst_mismatch_line=%" PRIu64
                   "\nlast_mismatch_index=%" PRIu64 "\n", v.first.index, v.first.line,
                   v.last_mismatch_index);
    }
    if (v.unchanged_violations > 0) {
      std::fprintf(out, "first_unchanged_violation_index=%" PRIu64 "\n",
                   v.first_unchanged_violation_index);
    }
  }
  if (r.has_stats) {
    const StatsResult& s = r.stats;
    std::fprintf(out, "stats_executed_bid=%" PRId64 "\nstats_executed_ask=%" PRId64
                 "\nstats_hidden_buy=%" PRId64 "\nstats_hidden_sell=%" PRId64
                 "\nstats_hidden_other=%" PRId64 "\nstats_two_sided_seconds=%.9g"
                 "\nstats_mean_spread_ticks=%.9g\nstats_mean_best_bid_qty=%.9g"
                 "\nstats_mean_best_ask_qty=%.9g\nstats_best_price_changes=%" PRIu64
                 "\nstats_best_quote_changes=%" PRIu64 "\nstats_frac_best_price_changes=%.9g"
                 "\nstats_frac_best_quote_changes=%.9g\n", s.executed_bid, s.executed_ask,
                 s.hidden_buy, s.hidden_sell, s.hidden_other, s.two_sided_seconds,
                 s.mean_spread_ticks, s.mean_best_bid_qty, s.mean_best_ask_qty,
                 s.best_price_changes, s.best_quote_changes, s.frac_best_price_changes(),
                 s.frac_best_quote_changes());
  }
  std::fprintf(out, "final_orders=%zu\n", r.final_orders);
}

}  // namespace lob::lobster
