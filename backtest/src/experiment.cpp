#include "lob/backtest/experiment.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>

#include "lob/paper_experiment.hpp"

namespace lob::backtest {

namespace {

std::vector<std::string> split_commas(const std::string& s) {
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

bool to_double(const std::string& s, double& out) {
  char* end = nullptr;
  errno = 0;
  out = std::strtod(s.c_str(), &end);
  return errno == 0 && end != s.c_str() && *end == '\0' && std::isfinite(out);
}

double mean_of(const std::vector<double>& v) {
  double s = 0;
  for (double x : v) s += x;
  return v.empty() ? 0.0 : s / static_cast<double>(v.size());
}

double sd_of(const std::vector<double>& v) {
  if (v.size() < 2) return 0.0;
  const double m = mean_of(v);
  double ss = 0;
  for (double x : v) ss += (x - m) * (x - m);
  return std::sqrt(ss / static_cast<double>(v.size() - 1));
}

const char* kind_name(StrategySpec::Kind k) {
  switch (k) {
    case StrategySpec::Kind::None: return "none";
    case StrategySpec::Kind::AS: return "as";
    case StrategySpec::Kind::Sym: return "sym";
    case StrategySpec::Kind::Naive: return "naive";
    case StrategySpec::Kind::Fixed: return "fixed";
    case StrategySpec::Kind::SymTV: return "symtv";
  }
  return "?";
}

}  // namespace

bool parse_strategies(const std::string& list, std::vector<StrategySpec>& out, std::string& err) {
  out.clear();
  for (const std::string& part : split_commas(list)) {
    StrategySpec s;
    s.label = part;
    const std::size_t colon = part.find(':');
    const std::string head = part.substr(0, colon);
    const std::string arg = colon == std::string::npos ? "" : part.substr(colon + 1);
    bool ok = true;
    if (head == "none" && arg.empty()) {
      s.kind = StrategySpec::Kind::None;
    } else if (head == "naive" && arg.empty()) {
      s.kind = StrategySpec::Kind::Naive;
    } else if (head == "as" || head == "sym" || head == "symtv") {
      s.kind = head == "as"    ? StrategySpec::Kind::AS
               : head == "sym" ? StrategySpec::Kind::Sym
                               : StrategySpec::Kind::SymTV;
      ok = to_double(arg, s.param) && s.param > 0.0;
    } else if (head == "fixed") {
      s.kind = StrategySpec::Kind::Fixed;
      ok = to_double(arg, s.param) && s.param >= 0.0;
    } else {
      ok = false;
    }
    if (!ok) {
      err = "bad strategy '" + part + "' (none, naive, as:<gamma>, sym:<gamma>, symtv:<gamma>, "
            "fixed:<ticks>)";
      return false;
    }
    out.push_back(s);
  }
  return true;
}

bool uses_as_inputs(StrategySpec::Kind k) noexcept {
  return k == StrategySpec::Kind::AS || k == StrategySpec::Kind::Sym ||
         k == StrategySpec::Kind::SymTV;
}

std::unique_ptr<Strategy> make_strategy(const StrategySpec& s, const SimParams& p,
                                        const AsCalibration& c) {
  AsParams a;
  a.gamma = s.param;
  a.sigma = c.sigma;
  a.k = c.k;
  a.horizon = p.horizon;
  a.requote = p.requote;
  switch (s.kind) {
    case StrategySpec::Kind::None: return std::make_unique<NeverQuote>();
    case StrategySpec::Kind::AS: return std::make_unique<AvellanedaStoikov>(a);
    case StrategySpec::Kind::Sym: return std::make_unique<SymmetricMatched>(a);
    case StrategySpec::Kind::Naive: return std::make_unique<NaiveTouch>();
    case StrategySpec::Kind::Fixed: return std::make_unique<FixedOffset>(s.param);
    case StrategySpec::Kind::SymTV: return std::make_unique<SymmetricTimeVarying>(a);
  }
  return std::make_unique<NeverQuote>();
}

CellResults run_cell(const SimParams& p, const std::vector<StrategySpec>& specs,
                     const AsCalibration& c, std::uint64_t seed, int episodes) {
  CellResults res(specs.size(), std::vector<EpisodeResult>(static_cast<std::size_t>(episodes)));
  for (int e = 0; e < episodes; ++e) {
    Market base(p, episode_seed(seed, static_cast<std::uint64_t>(e)));
    base.burn_in();
    for (std::size_t s = 0; s < specs.size(); ++s) {
      Market m = base;
      const auto strat = make_strategy(specs[s], p, c);
      res[s][static_cast<std::size_t>(e)] = m.run_episode(*strat);
    }
  }
  return res;
}

StrategySummary summarize(const std::vector<EpisodeResult>& eps, double horizon) {
  StrategySummary s;
  s.n = static_cast<int>(eps.size());
  if (eps.empty()) return s;
  std::vector<double> pnl, inv;
  double abs_inv = 0, fills = 0, lots = 0, qsec = 0, qdist = 0, captured = 0, disp = 0;
  double mk[kMarkoutHorizons.size()] = {};
  for (const EpisodeResult& r : eps) {
    pnl.push_back(r.pnl);
    inv.push_back(static_cast<double>(r.final_inventory));
    abs_inv += r.abs_inventory_time;
    s.max_abs_inv = std::max(s.max_abs_inv, static_cast<double>(r.max_abs_inventory));
    fills += static_cast<double>(r.fills);
    lots += static_cast<double>(r.fill_lots);
    qsec += r.quote_seconds;
    qdist += r.quote_dist_time;
    captured += r.spread_captured;
    for (std::size_t j = 0; j < kMarkoutHorizons.size(); ++j) mk[j] += r.markout_sum[j];
    if (std::isfinite(r.fill_dispersion)) {
      disp += r.fill_dispersion;
      ++s.dispersion_n;
    }
    s.post_only_rejects += static_cast<double>(r.post_only_rejects);
    s.limit_rejects += static_cast<double>(r.limit_rejects);
    s.invariant_failures += r.invariant_failures;
  }
  const double n = static_cast<double>(eps.size());
  s.mean_pnl = mean_of(pnl);
  s.sd_pnl = sd_of(pnl);
  s.sharpe = s.sd_pnl > 0 ? s.mean_pnl / s.sd_pnl : 0.0;
  s.mean_final_inv = mean_of(inv);
  s.sd_final_inv = sd_of(inv);
  s.mean_abs_inv = abs_inv / (n * horizon);
  s.mean_fills = fills / n;
  s.fill_rate = qsec > 0 ? fills / qsec : 0.0;
  s.spread_captured = lots > 0 ? captured / lots : 0.0;
  for (std::size_t j = 0; j < kMarkoutHorizons.size(); ++j) s.markout[j] = lots > 0 ? mk[j] / lots : 0.0;
  s.quote_dist = qsec > 0 ? qdist / qsec : 0.0;
  s.quote_share = qsec / (2.0 * horizon * n);
  s.fill_dispersion = s.dispersion_n > 0 ? disp / s.dispersion_n : std::nan("");
  s.post_only_rejects /= n;
  s.limit_rejects /= n;
  return s;
}

MarketDiagnostics diagnose(const std::vector<EpisodeResult>& eps, double horizon) {
  MarketDiagnostics d;
  if (eps.empty()) return d;
  double ss[kVolScales.size()] = {}, cnt[kVolScales.size()] = {};
  double spread = 0, depth = 0, two = 0, mos = 0, inf = 0, disp = 0;
  int disp_n = 0;
  for (const EpisodeResult& r : eps) {
    for (std::size_t j = 0; j < kVolScales.size(); ++j) {
      ss[j] += r.vol_ss[j];
      cnt[j] += static_cast<double>(r.vol_n[j]);
    }
    spread += r.spread_time;
    depth += r.touch_depth_time;
    two += r.two_sided_time;
    mos += static_cast<double>(r.market_orders);
    inf += static_cast<double>(r.informed_orders);
    if (std::isfinite(r.mo_dispersion)) {
      disp += r.mo_dispersion;
      ++disp_n;
    }
  }
  const double total = static_cast<double>(eps.size()) * horizon;
  for (std::size_t j = 0; j < kVolScales.size(); ++j) {
    d.sigma[j] = cnt[j] > 0 ? std::sqrt(ss[j] / (cnt[j] * kVolScales[j])) : 0.0;
  }
  d.spread = two > 0 ? spread / two : 0.0;
  d.touch_depth = two > 0 ? depth / two : 0.0;
  d.two_sided = two / total;
  d.mo_rate = mos / total;
  d.informed_rate = inf / total;
  d.mo_dispersion = disp_n > 0 ? disp / disp_n : std::nan("");
  return d;
}

KFit fit_k(const std::vector<EpisodeResult>& eps, double horizon, std::int64_t min_count,
           const std::vector<bool>* points) {
  KFit f;
  std::int64_t hist[kReachBins] = {};
  for (const EpisodeResult& r : eps) {
    for (std::size_t j = 0; j < kReachBins; ++j) hist[j] += r.reach_hist[j];
  }
  // Two sides, so the per-side rate divides by 2 * total time.
  const double per_side_time = 2.0 * static_cast<double>(eps.size()) * horizon;
  std::int64_t tail = 0;
  std::vector<std::int64_t> ge(kReachBins, 0);
  for (std::size_t j = kReachBins; j-- > 0;) {
    tail += hist[j];
    ge[j] = tail;
  }
  double sx = 0, sy = 0, sxx = 0, sxy = 0, syy = 0;
  int n = 0;
  for (std::size_t j = 1; j < kReachBins; ++j) {
    if (ge[j] == 0) break;
    const double delta = 0.5 * static_cast<double>(j);
    const double rate = static_cast<double>(ge[j]) / per_side_time;
    const std::size_t i = j - 1;
    const bool use = points ? (i < points->size() && (*points)[i]) : ge[j] >= min_count;
    f.delta.push_back(delta);
    f.rate.push_back(rate);
    f.count.push_back(ge[j]);
    f.used.push_back(use);
    if (!use) continue;
    const double y = std::log(rate);
    sx += delta;
    sy += y;
    sxx += delta * delta;
    sxy += delta * y;
    syy += y * y;
    ++n;
  }
  if (n >= 2) {
    const double dn = n;
    const double vx = sxx - sx * sx / dn, vy = syy - sy * sy / dn, cxy = sxy - sx * sy / dn;
    const double slope = cxy / vx;
    f.k = -slope;
    f.A = std::exp((sy - slope * sx) / dn);
    f.r2 = vy > 0 ? cxy * cxy / (vx * vy) : 1.0;
  }
  return f;
}

std::vector<CalibrationBlock> calibration_blocks(const std::vector<EpisodeResult>& eps,
                                                 double horizon, int blocks, const KFit& full) {
  std::vector<CalibrationBlock> out;
  const std::size_t n = eps.size();
  if (blocks < 1) return out;
  const auto nb = static_cast<std::size_t>(blocks);
  for (std::size_t b = 0; b < nb; ++b) {
    const std::size_t lo = b * n / nb, hi = (b + 1) * n / nb;
    const std::vector<EpisodeResult> sub(eps.begin() + static_cast<std::ptrdiff_t>(lo),
                                         eps.begin() + static_cast<std::ptrdiff_t>(hi));
    CalibrationBlock c;
    c.first = static_cast<int>(lo);
    c.episodes = static_cast<int>(hi - lo);
    c.sigma = diagnose(sub, horizon).sigma[2];  // the 10 s scale, as for the full estimate
    const KFit f = fit_k(sub, horizon, 0, &full.used);
    c.k = f.k;
    c.A = f.A;
    for (bool u : f.used) c.points += u ? 1 : 0;
    out.push_back(c);
  }
  return out;
}

namespace {

constexpr int kStats = 8;
const char* const kStatNames[kStats] = {"mean_pnl_diff",     "sd_pnl_ratio",   "sd_final_inv_ratio",
                                        "mean_abs_inv_diff", "sharpe_diff",    "fill_rate_ratio",
                                        "markout_1s_diff",   "markout_10s_diff"};

struct Acc {
  std::vector<double> pnl, inv;
  double abs_inv = 0, fills = 0, qsec = 0, lots = 0, mk1 = 0, mk10 = 0;
  void add(const EpisodeResult& r) {
    pnl.push_back(r.pnl);
    inv.push_back(static_cast<double>(r.final_inventory));
    abs_inv += r.abs_inventory_time;
    fills += static_cast<double>(r.fills);
    qsec += r.quote_seconds;
    lots += static_cast<double>(r.fill_lots);
    mk1 += r.markout_sum[1];
    mk10 += r.markout_sum[2];
  }
};

void stats_of(const std::vector<EpisodeResult>& a, const std::vector<EpisodeResult>& b,
              const std::vector<std::size_t>& idx, double horizon, double* out) {
  Acc x, y;
  for (std::size_t i : idx) {
    x.add(a[i]);
    y.add(b[i]);
  }
  const double n = static_cast<double>(idx.size());
  const double mx = mean_of(x.pnl), my = mean_of(y.pnl), sx = sd_of(x.pnl), sy = sd_of(y.pnl);
  const double nan = std::nan("");
  out[0] = mx - my;
  out[1] = sy > 0 ? sx / sy : nan;
  const double ix = sd_of(x.inv), iy = sd_of(y.inv);
  out[2] = iy > 0 ? ix / iy : nan;
  out[3] = (x.abs_inv - y.abs_inv) / (n * horizon);  // lots
  out[4] = (sx > 0 && sy > 0) ? mx / sx - my / sy : nan;
  const double fx = x.qsec > 0 ? x.fills / x.qsec : nan, fy = y.qsec > 0 ? y.fills / y.qsec : nan;
  out[5] = fy > 0 ? fx / fy : nan;
  out[6] = (x.lots > 0 && y.lots > 0) ? x.mk1 / x.lots - y.mk1 / y.lots : nan;
  out[7] = (x.lots > 0 && y.lots > 0) ? x.mk10 / x.lots - y.mk10 / y.lots : nan;
}

}  // namespace

std::vector<NamedInterval> paired_bootstrap(const std::vector<EpisodeResult>& a,
                                            const std::vector<EpisodeResult>& b, double horizon,
                                            int replicates, Rng& rng, double level) {
  const std::size_t n = a.size();
  std::vector<std::size_t> idx(n);
  for (std::size_t i = 0; i < n; ++i) idx[i] = i;
  double est[kStats];
  stats_of(a, b, idx, horizon, est);
  std::vector<std::vector<double>> reps(kStats);
  double v[kStats];
  for (int r = 0; r < replicates; ++r) {
    for (std::size_t i = 0; i < n; ++i) idx[i] = static_cast<std::size_t>(rng.uniform_int(n));
    stats_of(a, b, idx, horizon, v);
    for (int k = 0; k < kStats; ++k) {
      if (std::isfinite(v[k])) reps[static_cast<std::size_t>(k)].push_back(v[k]);
    }
  }
  std::vector<NamedInterval> out;
  for (int k = 0; k < kStats; ++k) {
    NamedInterval ni;
    ni.name = kStatNames[k];
    ni.iv.estimate = est[k];
    auto& r = reps[static_cast<std::size_t>(k)];
    ni.kept = static_cast<int>(r.size());
    std::sort(r.begin(), r.end());
    if (r.empty()) {
      ni.iv.lo = ni.iv.hi = std::nan("");
    } else {
      ni.iv.lo = paper::quantile_sorted(r, (1.0 - level) / 2.0);
      ni.iv.hi = paper::quantile_sorted(r, (1.0 + level) / 2.0);
    }
    out.push_back(ni);
  }
  return out;
}

void write_summary_header(std::FILE* f, const char* prefix) {
  std::fprintf(f,
               "%sstrategy,kind,param,n,mean_pnl,sd_pnl,sharpe,mean_final_inv,sd_final_inv,"
               "mean_abs_inv,max_abs_inv,mean_fills,fill_rate,spread_captured,markout_0.1s,"
               "markout_1s,markout_10s,quote_dist,quote_share,fill_dispersion,dispersion_n,"
               "post_only_rejects,limit_rejects,invariant_failures\n",
               prefix);
}

void write_summary_row(std::FILE* f, const std::string& prefix, const StrategySpec& spec,
                       const StrategySummary& s) {
  std::fprintf(f,
               "%s%s,%s,%.10g,%d,%.10g,%.10g,%.10g,%.10g,%.10g,%.10g,%.10g,%.10g,%.10g,%.10g,"
               "%.10g,%.10g,%.10g,%.10g,%.10g,%.10g,%d,%.10g,%.10g,%lld\n",
               prefix.c_str(), spec.label.c_str(), kind_name(spec.kind), spec.param, s.n,
               s.mean_pnl, s.sd_pnl, s.sharpe, s.mean_final_inv, s.sd_final_inv, s.mean_abs_inv,
               s.max_abs_inv, s.mean_fills, s.fill_rate, s.spread_captured, s.markout[0],
               s.markout[1], s.markout[2], s.quote_dist, s.quote_share, s.fill_dispersion,
               s.dispersion_n, s.post_only_rejects, s.limit_rejects,
               static_cast<long long>(s.invariant_failures));
}

void write_diagnostics_header(std::FILE* f, const char* prefix) {
  std::fprintf(f,
               "%sstrategy,sigma_0.1s,sigma_1s,sigma_10s,sigma_60s,spread,touch_depth,two_sided,"
               "mo_rate,informed_rate,mo_dispersion\n",
               prefix);
}

void write_diagnostics_row(std::FILE* f, const std::string& prefix, const StrategySpec& spec,
                           const MarketDiagnostics& d) {
  std::fprintf(f, "%s%s,%.10g,%.10g,%.10g,%.10g,%.10g,%.10g,%.10g,%.10g,%.10g,%.10g\n",
               prefix.c_str(), spec.label.c_str(), d.sigma[0], d.sigma[1], d.sigma[2], d.sigma[3],
               d.spread, d.touch_depth, d.two_sided, d.mo_rate, d.informed_rate, d.mo_dispersion);
}

std::vector<std::pair<std::size_t, std::size_t>> comparison_pairs(
    const std::vector<StrategySpec>& specs) {
  std::vector<std::pair<std::size_t, std::size_t>> out;
  auto find = [&](StrategySpec::Kind k, double param) -> long {
    for (std::size_t i = 0; i < specs.size(); ++i) {
      if (specs[i].kind == k && (k == StrategySpec::Kind::Naive || specs[i].param == param))
        return static_cast<long>(i);
    }
    return -1;
  };
  const long naive = find(StrategySpec::Kind::Naive, 0);
  for (std::size_t i = 0; i < specs.size(); ++i) {
    if (specs[i].kind != StrategySpec::Kind::AS) continue;
    const long sym = find(StrategySpec::Kind::Sym, specs[i].param);
    if (sym >= 0) out.emplace_back(i, static_cast<std::size_t>(sym));
    if (naive >= 0) out.emplace_back(i, static_cast<std::size_t>(naive));
  }
  for (std::size_t i = 0; i < specs.size(); ++i) {
    if (specs[i].kind == StrategySpec::Kind::Sym && naive >= 0)
      out.emplace_back(i, static_cast<std::size_t>(naive));
  }
  for (std::size_t i = 0; i < specs.size(); ++i) {
    if (specs[i].kind != StrategySpec::Kind::AS) continue;
    const long tv = find(StrategySpec::Kind::SymTV, specs[i].param);
    if (tv >= 0) out.emplace_back(i, static_cast<std::size_t>(tv));
  }
  for (std::size_t i = 0; i < specs.size(); ++i) {
    if (specs[i].kind != StrategySpec::Kind::SymTV) continue;
    const long sym = find(StrategySpec::Kind::Sym, specs[i].param);
    if (sym >= 0) out.emplace_back(i, static_cast<std::size_t>(sym));
  }
  return out;
}

}  // namespace lob::backtest
