// backtest_run: one market configuration, several strategies on the same episode seeds.
// Writes run.txt, summary.csv, diagnostics.csv, comparison.csv and, with --calibrate 1,
// calibration.csv and reach.csv. Run with --help for the flags; outputs are described in
// backtest/README.md.
#include <chrono>
#include <cstdio>

#include "cli.hpp"
#include "lob/backtest/experiment.hpp"

using namespace lob::backtest;

int main(int argc, char** argv) {
  cli::Options o;
  if (!cli::parse(argc, argv, o, false)) {
    cli::usage(stderr, argv[0], false);
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
      std::fprintf(stderr, "as/sym/symtv strategies need --as-sigma and --as-k\n");
      return 2;
    }
  }
  if (!cli::make_dir(o.out_dir)) return 1;

  const std::string load_start = cli::loadavg();
  const auto t0 = std::chrono::steady_clock::now();
  const CellResults res = run_cell(o.sim, specs, o.calib, o.seed, o.episodes);
  const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

  if (std::FILE* f = cli::open_out(o.out_dir, "summary.csv")) {
    write_summary_header(f, "");
    for (std::size_t s = 0; s < specs.size(); ++s) {
      const StrategySummary sm = summarize(res[s], o.sim.horizon);
      write_summary_row(f, "", specs[s], sm);
      std::printf("%-14s pnl %9.3f sd %8.3f fills %7.1f rate %.4f mk1s %+.3f |q| %.2f\n",
                  specs[s].label.c_str(), sm.mean_pnl, sm.sd_pnl, sm.mean_fills, sm.fill_rate,
                  sm.markout[1], sm.mean_abs_inv);
    }
    std::fclose(f);
  }
  if (std::FILE* f = cli::open_out(o.out_dir, "diagnostics.csv")) {
    write_diagnostics_header(f, "");
    for (std::size_t s = 0; s < specs.size(); ++s)
      write_diagnostics_row(f, "", specs[s], diagnose(res[s], o.sim.horizon));
    std::fclose(f);
  }
  if (o.bootstrap > 0) {
    if (std::FILE* f = cli::open_out(o.out_dir, "comparison.csv")) {
      std::fprintf(f, "a,b,replicates,statistic,estimate,ci_lo,ci_hi\n");
      cli::write_comparisons(f, "", specs, res, o.sim.horizon, o.bootstrap, o.seed, 0);
      std::fclose(f);
    }
  }
  if (o.calibrate) {
    long none = -1;
    for (std::size_t s = 0; s < specs.size(); ++s)
      if (specs[s].kind == StrategySpec::Kind::None) none = static_cast<long>(s);
    if (none < 0) {
      std::fprintf(stderr, "--calibrate 1 needs strategy 'none' in the list\n");
      return 2;
    }
    const auto& eps = res[static_cast<std::size_t>(none)];
    const MarketDiagnostics d = diagnose(eps, o.sim.horizon);
    // Minimum count for a k-fit point: 400 orders keeps the Poisson relative error of each rate
    // at 5% or less.
    const KFit kf = fit_k(eps, o.sim.horizon, 400);
    if (std::FILE* f = cli::open_out(o.out_dir, "calibration.csv")) {
      std::fprintf(f, "key,value\n");
      for (std::size_t j = 0; j < kVolScales.size(); ++j)
        std::fprintf(f, "sigma_%gs,%.17g\n", kVolScales[j], d.sigma[j]);
      std::fprintf(f, "sigma,%.17g\n", d.sigma[2]);  // the 10 s scale is the estimate used
      std::fprintf(f, "sigma_scale_s,%g\n", kVolScales[2]);
      std::fprintf(f, "k,%.17g\nA,%.17g\nk_fit_r2,%.17g\n", kf.k, kf.A, kf.r2);
      std::fprintf(f, "episodes,%d\nseed,%llu\nhorizon,%g\n", o.episodes,
                   static_cast<unsigned long long>(o.seed), o.sim.horizon);
      std::fclose(f);
    }
    if (std::FILE* f = cli::open_out(o.out_dir, "reach.csv")) {
      std::fprintf(f, "delta,rate,count,used\n");
      for (std::size_t j = 0; j < kf.delta.size(); ++j)
        std::fprintf(f, "%g,%.10g,%lld,%d\n", kf.delta[j], kf.rate[j],
                     static_cast<long long>(kf.count[j]), kf.used[j] ? 1 : 0);
      std::fclose(f);
    }
    std::printf("calibration: sigma(10s) %.6f ticks/sqrt(s), k %.6f /tick, A %.6f /s, r2 %.4f\n",
                d.sigma[2], kf.k, kf.A, kf.r2);
  }
  if (std::FILE* f = cli::open_out(o.out_dir, "run.txt")) {
    cli::write_run_txt(f, argc, argv, o);
    std::fprintf(f, "wall_seconds: %.1f\nloadavg_start: %s\nloadavg_end: %s\n", wall,
                 load_start.c_str(), cli::loadavg().c_str());
    std::fclose(f);
  }
  return 0;
}
