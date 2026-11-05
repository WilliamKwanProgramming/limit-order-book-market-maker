// Command-line handling shared by backtest_run and stress_test (not a public header).
#pragma once

#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "lob/backtest/experiment.hpp"
#include "lob/backtest/params.hpp"

namespace lob::backtest::cli {

struct Options {
  SimParams sim;
  std::string strategies = "as:0.01,sym:0.01,naive";
  int episodes = 100;
  std::uint64_t seed = 1;
  std::string out_dir = "results/backtest/run";
  int bootstrap = 1000;
  AsCalibration calib{1.0, 1.0};
  bool calib_given = false;
  bool calibrate = false;
  std::string list_params;  // write the parameter table to this file and exit
  std::vector<double> nu_grid{0.0};
  std::vector<double> nbr_grid{0.5};
};

inline bool parse_double(const char* s, double& out) {
  char* end = nullptr;
  errno = 0;
  out = std::strtod(s, &end);
  return errno == 0 && end != s && *end == '\0' && std::isfinite(out);
}

inline bool parse_long(const char* s, long long& out) {
  char* end = nullptr;
  errno = 0;
  out = std::strtoll(s, &end, 10);
  return errno == 0 && end != s && *end == '\0';
}

inline bool parse_list(const char* s, std::vector<double>& out) {
  out.clear();
  std::string str(s);
  std::size_t start = 0;
  while (start <= str.size()) {
    const std::size_t comma = str.find(',', start);
    const std::size_t stop = comma == std::string::npos ? str.size() : comma;
    double v = 0;
    if (!parse_double(str.substr(start, stop - start).c_str(), v)) return false;
    out.push_back(v);
    start = stop + 1;
  }
  return !out.empty();
}

inline void usage(std::FILE* f, const char* prog, bool grid) {
  std::fprintf(f, "usage: %s [options]\n", prog);
  std::fprintf(f,
               "  --strategies LIST  none, naive, as:<gamma>, sym:<gamma>, fixed:<ticks>\n"
               "  --episodes N       episodes per strategy%s (default 100)\n"
               "  --seed S           master seed (default 1)\n"
               "  --out DIR          output directory\n"
               "  --bootstrap B      paired bootstrap replicates, 0 to skip (default 1000)\n"
               "  --as-sigma X       A&S sigma estimate, ticks/sqrt(s) (required for as/sym)\n"
               "  --as-k X           A&S k estimate, 1/tick (required for as/sym)\n"
               "  --calibrate 0|1    also write calibration.csv and reach.csv from strategy none\n"
               "  --list-params FILE write the parameter table (CSV) and exit\n",
               grid ? " and grid cell" : "");
  if (grid) {
    std::fprintf(f,
                 "  --nu-grid LIST     news rates, 1/s (0 = no informed flow; run once)\n"
                 "  --nbr-grid LIST    branching ratios in [0, 1)\n");
  }
  std::fprintf(f, "model parameters (see backtest/README.md):\n");
  SimParams tmp;
  for (const ParamField& pf : param_fields(tmp)) {
    if (pf.d) std::fprintf(f, "  --%-15s %-12s (default %g)\n", pf.name, pf.unit, *pf.d);
    if (pf.i) std::fprintf(f, "  --%-15s %-12s (default %lld)\n", pf.name, pf.unit,
                           static_cast<long long>(*pf.i));
  }
}

inline bool parse(int argc, char** argv, Options& o, bool grid) {
  bool sigma_set = false, k_set = false;
  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];
    if (flag == "--help" || flag == "-h") {
      usage(stdout, argv[0], grid);
      std::exit(0);
    }
    if (i + 1 >= argc || flag.rfind("--", 0) != 0) {
      std::fprintf(stderr, "bad or incomplete flag: %s\n", flag.c_str());
      return false;
    }
    const char* v = argv[++i];
    long long n = 0;
    double x = 0;
    const std::string name = flag.substr(2);
    bool matched = false, ok = true;
    for (const ParamField& pf : param_fields(o.sim)) {
      if (name != pf.name) continue;
      matched = true;
      if (pf.d) ok = parse_double(v, *pf.d);
      if (pf.i) {
        ok = parse_long(v, n);
        *pf.i = n;
      }
    }
    if (matched) {
      if (!ok) {
        std::fprintf(stderr, "bad value for %s: %s\n", flag.c_str(), v);
        return false;
      }
      continue;
    }
    if (flag == "--strategies") {
      o.strategies = v;
    } else if (flag == "--episodes" && parse_long(v, n) && n >= 2 && n <= 10'000'000) {
      o.episodes = static_cast<int>(n);
    } else if (flag == "--seed" && parse_long(v, n) && n >= 0) {
      o.seed = static_cast<std::uint64_t>(n);
    } else if (flag == "--out") {
      o.out_dir = v;
    } else if (flag == "--bootstrap" && parse_long(v, n) && n >= 0 && n <= 1'000'000) {
      o.bootstrap = static_cast<int>(n);
    } else if (flag == "--as-sigma" && parse_double(v, x) && x > 0) {
      o.calib.sigma = x;
      sigma_set = true;
    } else if (flag == "--as-k" && parse_double(v, x) && x > 0) {
      o.calib.k = x;
      k_set = true;
    } else if (flag == "--calibrate" && parse_long(v, n) && (n == 0 || n == 1)) {
      o.calibrate = n == 1;
    } else if (flag == "--list-params") {
      o.list_params = v;
    } else if (grid && flag == "--nu-grid" && parse_list(v, o.nu_grid)) {
    } else if (grid && flag == "--nbr-grid" && parse_list(v, o.nbr_grid)) {
    } else {
      std::fprintf(stderr, "bad flag or value: %s %s\n", flag.c_str(), v);
      return false;
    }
  }
  o.calib_given = sigma_set && k_set;
  const std::string err = validate(o.sim);
  if (!err.empty()) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return false;
  }
  return true;
}

inline std::FILE* open_out(const std::string& dir, const char* name) {
  const std::string path = dir + "/" + name;
  std::FILE* f = std::fopen(path.c_str(), "w");
  if (!f) std::fprintf(stderr, "cannot write %s\n", path.c_str());
  return f;
}

inline bool make_dir(const std::string& dir) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) std::fprintf(stderr, "cannot create %s: %s\n", dir.c_str(), ec.message().c_str());
  return !ec;
}

inline std::string loadavg() {
  std::ifstream in("/proc/loadavg");
  std::string first;
  if (in >> first) return first;
  return "unknown";
}

inline bool write_param_table(const std::string& path) {
  std::FILE* f = std::fopen(path.c_str(), "w");
  if (!f) {
    std::fprintf(stderr, "cannot write %s\n", path.c_str());
    return false;
  }
  SimParams p;
  std::fprintf(f, "name,group,value,unit,rationale\n");
  for (const ParamField& pf : param_fields(p)) {
    if (pf.d) std::fprintf(f, "%s,%s,%g,%s,\"%s\"\n", pf.name, pf.group, *pf.d, pf.unit, pf.rationale);
    if (pf.i) std::fprintf(f, "%s,%s,%lld,%s,\"%s\"\n", pf.name, pf.group,
                           static_cast<long long>(*pf.i), pf.unit, pf.rationale);
  }
  std::fclose(f);
  return true;
}

inline void write_run_txt(std::FILE* f, int argc, char** argv, Options& o) {
  std::fprintf(f, "command:");
  for (int i = 0; i < argc; ++i) std::fprintf(f, " %s", argv[i]);
  std::fprintf(f, "\nseed: %llu\nepisodes: %d\nstrategies: %s\n",
               static_cast<unsigned long long>(o.seed), o.episodes, o.strategies.c_str());
  std::fprintf(f, "as_sigma: %.17g\nas_k: %.17g\n", o.calib.sigma, o.calib.k);
  std::fprintf(f, "params:");
  for (const ParamField& pf : param_fields(o.sim)) {
    if (pf.d) std::fprintf(f, " %s=%g", pf.name, *pf.d);
    if (pf.i) std::fprintf(f, " %s=%lld", pf.name, static_cast<long long>(*pf.i));
  }
  std::fprintf(f, "\n");
}

inline void write_comparisons(std::FILE* f, const std::string& prefix,
                              const std::vector<StrategySpec>& specs, const CellResults& res,
                              double horizon, int replicates, std::uint64_t seed,
                              std::uint64_t cell) {
  const auto pairs = comparison_pairs(specs);
  for (std::size_t k = 0; k < pairs.size(); ++k) {
    // Bootstrap streams far from the episode streams (0 .. episodes - 1).
    Rng rng = Rng::for_stream(seed, (std::uint64_t{1} << 62) + cell * 1000 + k);
    const auto& a = specs[pairs[k].first];
    const auto& b = specs[pairs[k].second];
    for (const NamedInterval& ni :
         paired_bootstrap(res[pairs[k].first], res[pairs[k].second], horizon, replicates, rng)) {
      std::fprintf(f, "%s%s,%s,%d,%s,%.10g,%.10g,%.10g\n", prefix.c_str(), a.label.c_str(),
                   b.label.c_str(), replicates, ni.name.c_str(), ni.iv.estimate, ni.iv.lo,
                   ni.iv.hi);
    }
  }
}

}  // namespace lob::backtest::cli
