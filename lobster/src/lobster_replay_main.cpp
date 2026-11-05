// lobster_replay: replay a LOBSTER message file into lob::OrderBook and, given the order-book
// file, check the engine's book against it after every message. See lobster/README.md.
#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "lob/lobster_session.hpp"

#ifndef LOBSTER_BUILD_INFO
#define LOBSTER_BUILD_INFO "unknown"
#endif

namespace {

using lob::lobster::FileSource;
using lob::lobster::Policy;
using lob::lobster::SessionOptions;
using lob::lobster::SessionResult;
using lob::lobster::Stage;

constexpr const char* kUsage =
    "usage: lobster_replay --messages FILE [--book FILE] [--levels N]\n"
    "                      [--policy strict|lenient] [--bootstrap] [--stats]\n"
    "                      [--bench] [--bench-passes K] [--out DIR] [--quiet]\n"
    "\n"
    "  --messages FILE   LOBSTER message file (time,type,id,size,price,direction)\n"
    "  --book FILE       matching order-book file; the engine is checked against every row\n"
    "  --levels N        expected level count of the order-book file (default: inferred)\n"
    "  --policy P        strict (default): stop at the first anomaly or malformed row\n"
    "                    lenient: count anomalies and malformed rows, and continue\n"
    "  --bootstrap       HEURISTIC, off by default, untested on real data: seed the book with\n"
    "                    one ghost order per visible level of the first order-book row, and\n"
    "                    resolve unknown ids against them (needs --book)\n"
    "  --stats           print descriptive statistics of the replayed stream\n"
    "  --bench           time parse-only, parse+replay and parse+replay+validate passes\n"
    "  --bench-passes K  passes per stage for --bench (default 5)\n"
    "  --out DIR         write summary.txt (key=value) and, with --bench, bench.txt to DIR\n"
    "  --quiet           do not print the human-readable summary\n"
    "\n"
    "exit code: 0 ok; 1 anomaly or malformed row under strict, or any validation mismatch;\n"
    "           2 usage, I/O or alignment error\n";

std::string load_average() {
  std::FILE* f = std::fopen("/proc/loadavg", "r");
  if (f == nullptr) return "n/a";
  char buf[64] = {0};
  const bool ok = std::fscanf(f, "%63s", buf) == 1;
  std::fclose(f);
  return ok ? std::string(buf) : std::string("n/a");
}

bool parse_size(const char* s, std::size_t& out) {
  char* end = nullptr;
  const unsigned long long v = std::strtoull(s, &end, 10);
  if (end == s || *end != '\0') return false;
  out = static_cast<std::size_t>(v);
  return true;
}

struct BenchStage {
  const char* name;
  Stage stage;
  std::vector<double> rates;  // messages per second, one per pass
  std::uint64_t messages = 0;
};

}  // namespace

int main(int argc, char** argv) {
  std::string messages_path;
  std::string book_path;
  std::string out_dir;
  SessionOptions opt;
  bool bench = false;
  bool quiet = false;
  std::size_t bench_passes = 5;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    const auto value = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "lobster_replay: %s needs a value\n%s", name, kUsage);
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--messages") {
      messages_path = value("--messages");
    } else if (a == "--book") {
      book_path = value("--book");
    } else if (a == "--levels") {
      if (!parse_size(value("--levels"), opt.levels) || opt.levels == 0 ||
          opt.levels > lob::lobster::kMaxBookLevels) {
        std::fprintf(stderr, "lobster_replay: --levels must be 1..%zu\n",
                     lob::lobster::kMaxBookLevels);
        return 2;
      }
    } else if (a == "--policy") {
      const std::string p = value("--policy");
      if (p == "strict") {
        opt.policy = Policy::Strict;
      } else if (p == "lenient") {
        opt.policy = Policy::Lenient;
      } else {
        std::fprintf(stderr, "lobster_replay: --policy must be strict or lenient\n");
        return 2;
      }
    } else if (a == "--bootstrap") {
      opt.bootstrap = true;
    } else if (a == "--stats") {
      opt.stats = true;
    } else if (a == "--bench") {
      bench = true;
    } else if (a == "--bench-passes") {
      if (!parse_size(value("--bench-passes"), bench_passes) || bench_passes == 0) {
        std::fprintf(stderr, "lobster_replay: --bench-passes must be >= 1\n");
        return 2;
      }
    } else if (a == "--out") {
      out_dir = value("--out");
    } else if (a == "--quiet") {
      quiet = true;
    } else if (a == "--help" || a == "-h") {
      std::fputs(kUsage, stdout);
      return 0;
    } else {
      std::fprintf(stderr, "lobster_replay: unknown argument %s\n%s", a.c_str(), kUsage);
      return 2;
    }
  }
  if (messages_path.empty()) {
    std::fprintf(stderr, "lobster_replay: --messages is required\n%s", kUsage);
    return 2;
  }
  if (opt.bootstrap && book_path.empty()) {
    std::fprintf(stderr, "lobster_replay: --bootstrap needs --book (it seeds from the first row)\n");
    return 2;
  }

  std::string command;
  for (int i = 0; i < argc; ++i) {
    if (i) command += ' ';
    command += argv[i];
  }

  // One full pass: the reported result.
  SessionResult result;
  {
    FileSource msgs(messages_path);
    if (!msgs.is_open()) {
      std::fprintf(stderr, "lobster_replay: cannot open %s\n", messages_path.c_str());
      return 2;
    }
    std::optional<FileSource> book;
    if (!book_path.empty()) {
      book.emplace(book_path);
      if (!book->is_open()) {
        std::fprintf(stderr, "lobster_replay: cannot open %s\n", book_path.c_str());
        return 2;
      }
    }
    result = lob::lobster::run_session(msgs, book ? &*book : nullptr, opt);
  }
  if (!quiet) {
    if (opt.bootstrap) {
      std::printf("NOTE: --bootstrap is a heuristic (ghost orders seeded from the first order-book"
                  " row). It has not been tested on real LOBSTER data.\n");
    }
    lob::lobster::print_summary(stdout, result);
  }

  // Benchmark passes over the same files (page cache warm after the first pass).
  std::vector<BenchStage> stages;
  std::string load_before;
  std::string load_after;
  if (bench) {
    stages.push_back({"parse", Stage::Parse, {}, 0});
    stages.push_back({"parse+replay", Stage::Replay, {}, 0});
    if (!book_path.empty()) stages.push_back({"parse+replay+validate", Stage::Validate, {}, 0});
    load_before = load_average();
    for (BenchStage& st : stages) {
      SessionOptions o = opt;
      o.stage = st.stage;
      o.stats = false;
      for (std::size_t p = 0; p < bench_passes; ++p) {
        FileSource msgs(messages_path);
        std::optional<FileSource> book;
        if (!book_path.empty()) book.emplace(book_path);
        const auto t0 = std::chrono::steady_clock::now();
        const SessionResult r = lob::lobster::run_session(msgs, book ? &*book : nullptr, o);
        const auto t1 = std::chrono::steady_clock::now();
        const double sec = std::chrono::duration<double>(t1 - t0).count();
        st.messages = r.message_records;
        st.rates.push_back(static_cast<double>(r.message_records) / sec);
      }
    }
    load_after = load_average();
  }
  const auto print_bench = [&](std::FILE* f) {
    std::fprintf(f, "build=%s\nbench_passes=%zu\nload_average_before=%s\nload_average_after=%s\n",
                 LOBSTER_BUILD_INFO, bench_passes, load_before.c_str(), load_after.c_str());
    for (BenchStage& st : stages) {
      std::vector<double> v = st.rates;
      std::sort(v.begin(), v.end());
      const double median = v.size() % 2 ? v[v.size() / 2]
                                          : 0.5 * (v[v.size() / 2 - 1] + v[v.size() / 2]);
      std::fprintf(f, "%s: messages=%" PRIu64 " msgs_per_s best=%.4g median=%.4g worst=%.4g\n",
                   st.name, st.messages, v.back(), median, v.front());
    }
  };
  if (bench && !quiet) {
    std::printf("--- bench (messages per second; first pass not excluded)\n");
    print_bench(stdout);
  }

  if (!out_dir.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);
    const std::string path = out_dir + "/summary.txt";
    std::FILE* f = std::fopen(path.c_str(), "w");
    if (f == nullptr) {
      std::fprintf(stderr, "lobster_replay: cannot write %s\n", path.c_str());
      return 2;
    }
    std::fprintf(f, "command=%s\nmessages_file=%s\nbook_file=%s\n", command.c_str(),
                 messages_path.c_str(), book_path.c_str());
    lob::lobster::print_key_values(f, result);
    std::fclose(f);
    if (bench) {
      const std::string bpath = out_dir + "/bench.txt";
      std::FILE* b = std::fopen(bpath.c_str(), "w");
      if (b == nullptr) {
        std::fprintf(stderr, "lobster_replay: cannot write %s\n", bpath.c_str());
        return 2;
      }
      std::fprintf(b, "command=%s\n", command.c_str());
      print_bench(b);
      std::fclose(b);
    }
  }
  return result.exit_code();
}
