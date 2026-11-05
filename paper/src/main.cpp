// paper_replication: Monte Carlo reproduction of the simulation study in Avellaneda and Stoikov
// (2008), Section 3.3 of the author's PDF. Run with --help for the flags. Output files are
// described in paper/README.md.
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "lob/avellaneda_stoikov.hpp"
#include "lob/paper_experiment.hpp"

namespace {

using namespace lob::paper;

// Bootstrap streams live far away from the path streams (which use ids 0 .. sims - 1).
constexpr std::uint64_t kBootstrapStreamBase = 1ULL << 62;

struct Options {
  ExperimentConfig cfg;
  std::string out_dir = "results/paper/run";
  int bootstrap = 2000;
  bool per_sim = true;
  int block_size = 0;
  long trace_sim = -1;
};

void usage(std::FILE* f) {
  std::fprintf(f,
               "usage: paper_replication [options]\n"
               "  --gamma LIST       comma-separated risk aversions (default 0.1,0.01,1)\n"
               "  --sims N           simulations per gamma (default 1000)\n"
               "  --seed S           master seed (default 1)\n"
               "  --strategy LIST    inventory,symmetric,symmetric_tv or all (default all)\n"
               "  --out DIR          output directory (default results/paper/run)\n"
               "  --bootstrap B      bootstrap replicates, 0 to skip (default 2000)\n"
               "  --per-sim 0|1      write sims.csv with one row per simulation (default 1)\n"
               "  --block-size M     also write statistics of consecutive blocks of M sims\n"
               "  --trace I          write the step-by-step trace of simulation I\n");
}

bool parse_double(const char* s, double& out) {
  char* end = nullptr;
  errno = 0;
  out = std::strtod(s, &end);
  return errno == 0 && end != s && *end == '\0';
}

bool parse_long(const char* s, long long& out) {
  char* end = nullptr;
  errno = 0;
  out = std::strtoll(s, &end, 10);
  return errno == 0 && end != s && *end == '\0';
}

std::vector<std::string> split(const std::string& s) {
  std::vector<std::string> parts;
  std::size_t start = 0;
  while (start <= s.size()) {
    const std::size_t comma = s.find(',', start);
    const std::size_t stop = comma == std::string::npos ? s.size() : comma;
    parts.push_back(s.substr(start, stop - start));
    start = stop + 1;
  }
  return parts;
}

bool parse_args(int argc, char** argv, Options& o) {
  o.cfg.gammas = {0.1, 0.01, 1.0};
  o.cfg.strategies = {Strategy::Inventory, Strategy::Symmetric, Strategy::SymmetricTimeMatched};
  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];
    if (flag == "--help" || flag == "-h") {
      usage(stdout);
      std::exit(0);
    }
    if (i + 1 >= argc) {
      std::fprintf(stderr, "missing value for %s\n", flag.c_str());
      return false;
    }
    const char* v = argv[++i];
    long long n = 0;
    if (flag == "--gamma") {
      o.cfg.gammas.clear();
      for (const std::string& part : split(v)) {
        double g = 0.0;
        if (!parse_double(part.c_str(), g) || !(g > 0.0)) {
          std::fprintf(stderr, "bad gamma '%s' (must be > 0)\n", part.c_str());
          return false;
        }
        o.cfg.gammas.push_back(g);
      }
    } else if (flag == "--strategy") {
      o.cfg.strategies.clear();
      if (std::strcmp(v, "all") == 0) {
        o.cfg.strategies = {Strategy::Inventory, Strategy::Symmetric,
                            Strategy::SymmetricTimeMatched};
        continue;
      }
      for (const std::string& part : split(v)) {
        Strategy s{};
        if (!parse_strategy(part.c_str(), s)) {
          std::fprintf(stderr, "unknown strategy '%s'\n", part.c_str());
          return false;
        }
        o.cfg.strategies.push_back(s);
      }
    } else if (flag == "--sims" && parse_long(v, n) && n >= 2 && n <= 100'000'000) {
      o.cfg.sims = static_cast<int>(n);
    } else if (flag == "--seed" && parse_long(v, n) && n >= 0) {
      o.cfg.seed = static_cast<std::uint64_t>(n);
    } else if (flag == "--out") {
      o.out_dir = v;
    } else if (flag == "--bootstrap" && parse_long(v, n) && n >= 0 && n <= 1'000'000) {
      o.bootstrap = static_cast<int>(n);
    } else if (flag == "--per-sim" && parse_long(v, n) && (n == 0 || n == 1)) {
      o.per_sim = n == 1;
    } else if (flag == "--block-size" && parse_long(v, n) && n >= 0) {
      o.block_size = static_cast<int>(n);
    } else if (flag == "--trace" && parse_long(v, n) && n >= 0) {
      o.trace_sim = static_cast<long>(n);
    } else {
      std::fprintf(stderr, "bad flag or value: %s %s\n", flag.c_str(), v);
      return false;
    }
  }
  if (o.trace_sim >= o.cfg.sims) {
    std::fprintf(stderr, "--trace index must be below --sims\n");
    return false;
  }
  return true;
}

std::FILE* open_out(const std::string& dir, const char* name) {
  const std::string path = dir + "/" + name;
  std::FILE* f = std::fopen(path.c_str(), "w");
  if (!f) std::fprintf(stderr, "cannot write %s\n", path.c_str());
  return f;
}

int find_strategy(const std::vector<Strategy>& v, Strategy s) {
  for (std::size_t i = 0; i < v.size(); ++i) {
    if (v[i] == s) return static_cast<int>(i);
  }
  return -1;
}

}  // namespace

int main(int argc, char** argv) {
  Options o;
  if (!parse_args(argc, argv, o)) {
    usage(stderr);
    return 2;
  }
  std::error_code ec;
  std::filesystem::create_directories(o.out_dir, ec);
  if (ec) {
    std::fprintf(stderr, "cannot create %s: %s\n", o.out_dir.c_str(), ec.message().c_str());
    return 1;
  }
  const ExperimentConfig& cfg = o.cfg;
  const ModelParams& p = cfg.model;
  const ExperimentResults res = run_experiment(cfg);

  if (std::FILE* f = open_out(o.out_dir, "run.txt")) {
    std::fprintf(f, "command:");
    for (int i = 0; i < argc; ++i) std::fprintf(f, " %s", argv[i]);
    std::fprintf(f, "\nseed: %llu\nsims: %d\n", static_cast<unsigned long long>(cfg.seed),
                 cfg.sims);
    std::fprintf(f, "model: s0=%g sigma=%g T=%g A=%g k=%g steps=%d dt=%g\n", p.s0, p.sigma,
                 p.horizon, p.A, p.k, p.steps, p.dt());
    std::fclose(f);
  }

  if (std::FILE* f = open_out(o.out_dir, "summary.csv")) {
    std::fprintf(f, "gamma,strategy,n,mean_spread,mean_profit,sd_profit,mean_final_q,sd_final_q\n");
    std::printf("%-6s %-13s %8s %9s %9s %8s %8s\n", "gamma", "strategy", "spread", "profit",
                "sd", "final_q", "sd_q");
    for (std::size_t g = 0; g < cfg.gammas.size(); ++g) {
      for (std::size_t s = 0; s < cfg.strategies.size(); ++s) {
        const Summary m = summarize(res[g][s]);
        const char* name = strategy_name(cfg.strategies[s]);
        std::fprintf(f, "%.10g,%s,%d,%.10g,%.10g,%.10g,%.10g,%.10g\n", cfg.gammas[g], name, m.n,
                     m.mean_spread, m.mean_profit, m.sd_profit, m.mean_q, m.sd_q);
        std::printf("%-6g %-13s %8.4f %9.3f %9.3f %8.3f %8.3f\n", cfg.gammas[g], name,
                    m.mean_spread, m.mean_profit, m.sd_profit, m.mean_q, m.sd_q);
      }
    }
    std::fclose(f);
  }

  const int inv = find_strategy(cfg.strategies, Strategy::Inventory);
  if (o.bootstrap > 0 && inv >= 0) {
    if (std::FILE* f = open_out(o.out_dir, "comparison.csv")) {
      std::fprintf(f, "gamma,baseline,replicates,statistic,estimate,ci_lo,ci_hi\n");
      for (std::size_t g = 0; g < cfg.gammas.size(); ++g) {
        for (std::size_t s = 0; s < cfg.strategies.size(); ++s) {
          if (static_cast<int>(s) == inv) continue;
          // One stream per (gamma, baseline), so adding a gamma does not change other intervals.
          lob::Rng rng = lob::Rng::for_stream(
              cfg.seed, kBootstrapStreamBase + 16 * g + static_cast<std::uint64_t>(cfg.strategies[s]));
          const PairedComparison c = paired_bootstrap(res[g][static_cast<std::size_t>(inv)],
                                                      res[g][s], o.bootstrap, rng);
          const char* base = strategy_name(cfg.strategies[s]);
          const std::pair<const char*, const Interval*> rows[] = {
              {"sd_profit_ratio", &c.sd_profit_ratio},
              {"sd_final_q_ratio", &c.sd_q_ratio},
              {"mean_profit_rel_diff", &c.mean_profit_rel_diff}};
          for (const auto& [stat, iv] : rows) {
            std::fprintf(f, "%.10g,%s,%d,%s,%.10g,%.10g,%.10g\n", cfg.gammas[g], base,
                         o.bootstrap, stat, iv->estimate, iv->lo, iv->hi);
          }
        }
      }
      std::fclose(f);
    }
  }

  if (o.per_sim) {
    if (std::FILE* f = open_out(o.out_dir, "sims.csv")) {
      std::fprintf(f, "gamma,strategy,sim,profit,final_q,cash,final_mid,bid_fills,ask_fills\n");
      for (std::size_t g = 0; g < cfg.gammas.size(); ++g) {
        for (std::size_t s = 0; s < cfg.strategies.size(); ++s) {
          const char* name = strategy_name(cfg.strategies[s]);
          for (std::size_t i = 0; i < res[g][s].size(); ++i) {
            const SimResult& r = res[g][s][i];
            std::fprintf(f, "%.10g,%s,%zu,%.10g,%lld,%.10g,%.10g,%d,%d\n", cfg.gammas[g], name, i,
                         r.profit, static_cast<long long>(r.final_q), r.cash, r.final_mid,
                         r.bid_fills, r.ask_fills);
          }
        }
      }
      std::fclose(f);
    }
  }

  if (o.block_size > 0) {
    if (std::FILE* f = open_out(o.out_dir, "blocks.csv")) {
      std::fprintf(f, "gamma,strategy,block,n,mean_profit,sd_profit,mean_final_q,sd_final_q\n");
      const auto bs = static_cast<std::size_t>(o.block_size);
      for (std::size_t g = 0; g < cfg.gammas.size(); ++g) {
        for (std::size_t s = 0; s < cfg.strategies.size(); ++s) {
          const char* name = strategy_name(cfg.strategies[s]);
          for (std::size_t b = 0; (b + 1) * bs <= res[g][s].size(); ++b) {
            const Summary m = summarize(res[g][s], b * bs, bs);
            std::fprintf(f, "%.10g,%s,%zu,%d,%.10g,%.10g,%.10g,%.10g\n", cfg.gammas[g], name, b,
                         m.n, m.mean_profit, m.sd_profit, m.mean_q, m.sd_q);
          }
        }
      }
      std::fclose(f);
    }
  }

  if (o.trace_sim >= 0) {
    if (std::FILE* f = open_out(o.out_dir, "trace.csv")) {
      std::fprintf(f, "gamma,strategy,step,t,mid,bid,ask,q,cash,bid_fill,ask_fill\n");
      Path path;
      generate_path(cfg.seed, static_cast<std::uint64_t>(o.trace_sim), p, path);
      std::vector<TraceRow> trace;
      for (double gamma : cfg.gammas) {
        for (Strategy s : cfg.strategies) {
          trace.clear();
          const SimResult r = simulate(path, s, gamma, p, &trace);
          for (std::size_t n = 0; n < trace.size(); ++n) {
            const TraceRow& t = trace[n];
            std::fprintf(f, "%.10g,%s,%zu,%.10g,%.10g,%.10g,%.10g,%lld,%.10g,%d,%d\n", gamma,
                         strategy_name(s), n, t.t, t.mid, t.bid, t.ask,
                         static_cast<long long>(t.q), t.cash, t.bid_fill ? 1 : 0,
                         t.ask_fill ? 1 : 0);
          }
          // Terminal row: state after the last step, at t = T and the final mid.
          std::fprintf(f, "%.10g,%s,%d,%.10g,%.10g,,,%lld,%.10g,0,0\n", gamma, strategy_name(s),
                       p.steps, p.horizon, r.final_mid, static_cast<long long>(r.final_q),
                       r.cash);
        }
      }
      std::fclose(f);
    }
  }
  return 0;
}
