#!/usr/bin/env bash
# Regenerates every number and figure under results/backtest/ (about 10 minutes on one core).
# Usage: backtest/reproduce.sh [build-dir]   (default build-backtest; binaries must be built)
set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo"  # every path below is relative to the repository root, so none is recorded in results/
build="${1:-build-backtest}"
bin="$build/backtest"
out="results/backtest"

for b in backtest_run stress_test; do
  [[ -x "$bin/$b" ]] || { echo "missing $bin/$b: build it first (see backtest/README.md)" >&2; exit 1; }
done
mkdir -p "$out"
load_start="$(cut -d' ' -f1 /proc/loadavg)"

# Episodes per strategy (and per stress cell). Seeds are fixed in advance, one per experiment.
N=2000

# 0. Parameter table (defaults, units, rationale) for the README.
"$bin/backtest_run" --list-params "$out/params.csv"

# 1. Calibration run of the uninformed market (no strategy, plus fixed-offset probes):
#    sigma from 10 s mid returns, k from the market-order reach. Seed 101.
"$bin/backtest_run" --strategies none,fixed:0.5,fixed:1.5,fixed:2.5,fixed:3.5 \
  --episodes 1000 --seed 101 --bootstrap 0 --calibrate 1 --out "$out/calibration"
sigma="$(awk -F, '$1=="sigma"{print $2}' "$out/calibration/calibration.csv")"
k="$(awk -F, '$1=="k"{print $2}' "$out/calibration/calibration.csv")"

# 2. Gamma grid: gamma = c / (sigma^2 T / 2), i.e. one lot of inventory moves the reservation
#    price by c ticks at mid-horizon, c = 0.01, 0.03, 0.1, 0.3, 1 (half-decade grid).
#    The stress test uses the middle value, c = 0.1.
read -r grid gmid < <(python3 -c "
s, T = $sigma, 600.0
g = [c / (s * s * T / 2) for c in (0.01, 0.03, 0.1, 0.3, 1.0)]
print(','.join(f'as:{x:.6g},sym:{x:.6g}' for x in g), f'{g[2]:.6g}')")

# 3. Uninformed baseline: gamma sweep against the symmetric benchmark and the naive quoter. Seed 202.
"$bin/backtest_run" --strategies "none,naive,$grid" --episodes $N --seed 202 --bootstrap 1000 \
  --as-sigma "$sigma" --as-k "$k" --out "$out/baseline"

# 4. Stress grid: informed Hawkes flow, news rate x branching ratio (plus no informed flow). Seed 303.
"$bin/stress_test" --strategies "none,naive,as:$gmid,sym:$gmid" \
  --nu-grid 0,0.01,0.03,0.1 --nbr-grid 0.5,0.75,0.9 --episodes $N --seed 303 --bootstrap 1000 \
  --as-sigma "$sigma" --as-k "$k" --out "$out/stress"

{
  echo "generated: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "commit: $(git rev-parse HEAD 2>/dev/null || echo unknown)"
  if [[ -n "$(git status --porcelain -- backtest 2>/dev/null)" ]]; then
    echo "working tree: backtest/ has uncommitted changes relative to that commit"
  fi
  echo "compiler: $(grep -m1 CMAKE_CXX_COMPILER: "$build/CMakeCache.txt" | cut -d= -f2)"
  echo "build type: $(grep -m1 CMAKE_BUILD_TYPE: "$build/CMakeCache.txt" | cut -d= -f2)"
  echo "machine: $(nproc) cores, $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | sed 's/^ //')"
  echo "load average (1 min) at start: $load_start, at end: $(cut -d' ' -f1 /proc/loadavg)"
  echo "episodes per strategy: $N (calibration 1000)"
  echo "A&S estimates: sigma=$sigma ticks/sqrt(s), k=$k 1/tick"
  for d in calibration baseline stress; do echo "--- $d"; cat "$out/$d/run.txt"; done
} > "$out/MANIFEST.txt"

python3 backtest/make_tables.py
python3 backtest/plot.py
