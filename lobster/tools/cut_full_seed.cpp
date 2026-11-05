// lobster_cut_full_seed: the cut experiment with the bootstrap seeded from the complete book.
//
// lobster_replay --policy lenient --bootstrap seeds one ghost order per level from the first row of
// the order-book file, which shows only N levels. This tool runs the same replay and validation
// with one difference: the ghosts are seeded from a row that holds every level of the book after
// message 1 (synth_lobster.py --full-row), so seed_ghosts() undoes message 1 on the complete book
// and rests one ghost per level of the complete book at the cut, with the level's true size. If
// the mismatches that remain after the usual bootstrap come only from levels deeper than N at the
// start, none should remain here. Everything else (policy lenient, Replayer, Validator, the N-level
// rows checked after every message) is what lobster_replay does. See lobster/README.md.
//
// usage: lobster_cut_full_seed --messages FILE --book FILE --full-row FILE --out DIR
// Writes DIR/summary.txt in the key=value format of lobster_replay --out, plus full_row_levels.
// Exit code: 0 if no row mismatches, 1 if some do, 2 for usage, I/O or alignment errors.
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>

#include "lob/lobster_session.hpp"

namespace {

using namespace lob::lobster;

int fail(const std::string& what) {
  std::fprintf(stderr, "lobster_cut_full_seed: %s\n", what.c_str());
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  std::string messages_path;
  std::string book_path;
  std::string full_path;
  std::string out_dir;
  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    if (i + 1 >= argc) return fail(std::string("missing value after ") + a);
    if (std::strcmp(a, "--messages") == 0) {
      messages_path = argv[++i];
    } else if (std::strcmp(a, "--book") == 0) {
      book_path = argv[++i];
    } else if (std::strcmp(a, "--full-row") == 0) {
      full_path = argv[++i];
    } else if (std::strcmp(a, "--out") == 0) {
      out_dir = argv[++i];
    } else {
      return fail(std::string("unknown argument ") + a);
    }
  }
  if (messages_path.empty() || book_path.empty() || full_path.empty() || out_dir.empty()) {
    return fail("usage: --messages FILE --book FILE --full-row FILE --out DIR");
  }
  FileSource msrc(messages_path);
  FileSource bsrc(book_path);
  FileSource fsrc(full_path);
  if (!msrc.is_open() || !bsrc.is_open() || !fsrc.is_open()) return fail("cannot open an input");

  BookRow full;
  {
    BookReader fr(fsrc);
    if (fr.next(full) != ReadResult::Record) return fail("cannot read the full row");
  }

  SessionResult r;
  r.options.policy = Policy::Lenient;
  r.options.bootstrap = true;
  r.has_book = true;
  MessageReader mr(msrc);
  BookReader br(bsrc);
  Replayer rep(Replayer::Config{Policy::Lenient, true});
  std::optional<Validator> val;
  Message m;
  BookRow row;
  std::uint64_t index = 0;
  for (;;) {
    const ReadResult mres = mr.next(m);
    if (mres == ReadResult::End) break;
    ++index;
    ++r.message_records;
    const ReadResult bres = br.next(row);
    if (bres == ReadResult::End) return fail("order-book file has fewer rows than messages");
    const bool row_ok = bres == ReadResult::Record;
    if (!row_ok) ++r.book_parse_errors;
    if (!val && br.levels() > 0) val.emplace(br.levels());
    if (mres == ReadResult::Malformed) {
      ++r.message_parse_errors;
      if (val) val->skip(row_ok ? &row : nullptr);
      continue;
    }
    if (index == 1 && !rep.seed_ghosts(m, full)) return fail("seed_ghosts refused");
    rep.apply(m, index, mr.line());
    if (!val) continue;
    if (row_ok) {
      val->check(rep.book(), m, index, mr.line(), row);
    } else {
      val->skip(nullptr);
    }
  }
  if (mr.io_failed() || br.io_failed()) return fail("read error");
  if (index == 0) return fail("the message file has no rows");
  if (br.next(row) != ReadResult::End) return fail("order-book file has more rows than messages");
  r.levels = br.levels();
  r.message_blank_lines = mr.blank_lines();
  r.book_blank_lines = br.blank_lines();
  r.replay = rep.counters();
  r.ghosts = rep.ghosts();
  r.first_anomaly = rep.first_anomaly();
  if (val) {
    r.validated = true;
    r.validation = val->report();
  }
  r.final_orders = rep.book().order_count();

  std::error_code ec;
  std::filesystem::create_directories(out_dir, ec);
  const std::string path = out_dir + "/summary.txt";
  std::FILE* f = std::fopen(path.c_str(), "w");
  if (f == nullptr) return fail("cannot write " + path);
  std::fprintf(f, "command=lobster_cut_full_seed --messages %s --book %s --full-row %s --out %s\n",
               messages_path.c_str(), book_path.c_str(), full_path.c_str(), out_dir.c_str());
  std::fprintf(f, "full_row_levels=%zu\n", full.levels());
  print_key_values(f, r);
  std::fclose(f);
  print_summary(stdout, r);
  return r.validated && r.validation.mismatched == 0 ? 0 : 1;
}
