// stress_test: the strategies of backtest_run across a grid of informed-flow intensities
// (news rate nu x branching ratio n_br, plus nu = 0 run once). Every cell uses the same episode
// seeds, so background flow is common across cells as well as across strategies.
// Writes run.txt, stress_summary.csv, stress_diagnostics.csv, stress_comparison.csv and, when the
// list has as, skewc, symtv and sym at one gamma, stress_factorial.csv.
#include <chrono>
#include <cstdio>

#include "cli.hpp"
#include "lob/backtest/experiment.hpp"

using namespace lob::backtest;

int main(int argc, char** argv) {
  cli::Options o;
  if (!cli::parse(argc, argv, o, true)) {
    cli::usage(stderr, argv[0], true);
    return 2;
  }
  if (!o.list_params.empty()) return cli::write_param_table(o.list_params) ? 0 : 1;
  std::vector<StrategySpec> specs;
  std::string err;
  if (!parse_strategies(o.strategies, specs, err)) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return 2;
  }
  for (const StrategySpec& s : specs) {
    if (uses_as_inputs(s.kind) && !o.calib_given) {
      std::fprintf(stderr, "as/sym/symtv/skewc strategies need --as-sigma and --as-k\n");
      return 2;
    }
  }
  for (double nb : o.nbr_grid) {
    if (!(nb >= 0.0 && nb < 1.0)) {
      std::fprintf(stderr, "branching ratios must be in [0, 1)\n");
      return 2;
    }
  }
  if (!cli::make_dir(o.out_dir)) return 1;

  struct Cell {
    double nu, nbr;
  };
  std::vector<Cell> cells;
  for (double nu : o.nu_grid) {
    if (nu < 0) {
      std::fprintf(stderr, "news rates must be >= 0\n");
      return 2;
    }
    if (nu == 0.0) {
      cells.push_back({0.0, 0.0});
    } else {
      for (double nb : o.nbr_grid) cells.push_back({nu, nb});
    }
  }

  std::FILE* fs = cli::open_out(o.out_dir, "stress_summary.csv");
  std::FILE* fd = cli::open_out(o.out_dir, "stress_diagnostics.csv");
  std::FILE* fc = o.bootstrap > 0 ? cli::open_out(o.out_dir, "stress_comparison.csv") : nullptr;
  const bool factorial = o.bootstrap > 0 && cli::has_factorial(specs);
  std::FILE* ff = factorial ? cli::open_out(o.out_dir, "stress_factorial.csv") : nullptr;
  if (!fs || !fd || (o.bootstrap > 0 && !fc) || (factorial && !ff)) return 1;
  write_summary_header(fs, "nu,n_br,");
  write_diagnostics_header(fd, "nu,n_br,");
  if (fc) {
    std::fprintf(fc, "nu,n_br,a,b,replicates,statistic,estimate,ci_lo,ci_hi,replicates_kept\n");
  }
  if (ff) {
    std::fprintf(ff, "nu,n_br,as,skewc,symtv,sym,replicates,statistic,estimate,ci_lo,ci_hi,"
                     "replicates_kept\n");
  }

  const std::string load_start = cli::loadavg();
  const auto t0 = std::chrono::steady_clock::now();
  for (std::size_t c = 0; c < cells.size(); ++c) {
    SimParams p = o.sim;
    p.news_rate = cells[c].nu;
    p.branching = cells[c].nbr;
    const CellResults res = run_cell(p, specs, o.as_inputs(), o.seed, o.episodes);
    char prefix[64];
    std::snprintf(prefix, sizeof prefix, "%g,%g,", cells[c].nu, cells[c].nbr);
    for (std::size_t s = 0; s < specs.size(); ++s) {
      const StrategySummary sm = summarize(res[s], p.horizon);
      write_summary_row(fs, prefix, specs[s], sm);
      write_diagnostics_row(fd, prefix, specs[s], diagnose(res[s], p.horizon));
      std::printf("nu %-5g n_br %-5g %-14s pnl %9.3f sd %8.3f fills %7.1f mk1s %+.3f\n",
                  cells[c].nu, cells[c].nbr, specs[s].label.c_str(), sm.mean_pnl, sm.sd_pnl,
                  sm.mean_fills, sm.markout[1]);
    }
    if (fc) cli::write_comparisons(fc, prefix, specs, res, p.horizon, o.bootstrap, o.seed, c);
    if (ff) cli::write_factorial(ff, prefix, specs, res, o.bootstrap, o.seed, c);
    std::fflush(stdout);
  }
  const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::fclose(fs);
  std::fclose(fd);
  if (fc) std::fclose(fc);
  if (ff) std::fclose(ff);
  if (std::FILE* f = cli::open_out(o.out_dir, "run.txt")) {
    cli::write_run_txt(f, argc, argv, o);
    std::fprintf(f, "nu_grid:");
    for (double v : o.nu_grid) std::fprintf(f, " %g", v);
    std::fprintf(f, "\nnbr_grid:");
    for (double v : o.nbr_grid) std::fprintf(f, " %g", v);
    std::fprintf(f, "\nwall_seconds: %.1f\nloadavg_start: %s\nloadavg_end: %s\n", wall,
                 load_start.c_str(), cli::loadavg().c_str());
    std::fclose(f);
  }
  return 0;
}
