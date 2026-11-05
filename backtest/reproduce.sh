#!/usr/bin/env bash
# Regenerates every number and figure under results/backtest/ (about 20 minutes on one core).
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
# Recorded before anything is written (this script's own outputs would make the tree dirty).
if ! status="$(git status --porcelain 2>/dev/null)"; then
  tree="unknown (not a git checkout)"
elif [[ -n "$status" ]]; then
  tree="has uncommitted changes relative to that commit"
else
  tree="clean"
fi
mkdir -p "$out"
load_start="$(cut -d' ' -f1 /proc/loadavg)"

# Episodes per strategy (and per stress cell). Seeds are fixed in advance, one per experiment.
N=2000
# Episodes per cell and setting of the sensitivity runs (a compute-budget choice, half of N).
NS=1000
# Paired bootstrap replicates for every comparison.
B=1000

# 0. Parameter table (defaults, units, rationale) for the README.
"$bin/backtest_run" --list-params "$out/params.csv"

# 1. Calibration run of the uninformed market (no strategy, plus fixed-offset probes):
#    sigma from 10 s mid returns, k from the market-order reach. Seed 101. Also sigma and k on 10
#    blocks of 100 episodes, for the sampling spread of the estimates.
"$bin/backtest_run" --strategies none,fixed:0.5,fixed:1.5,fixed:2.5,fixed:3.5 \
  --episodes 1000 --seed 101 --bootstrap 0 --calibrate 1 --calib-blocks 10 --out "$out/calibration"
sigma="$(awk -F, '$1=="sigma"{print $2}' "$out/calibration/calibration.csv")"
k="$(awk -F, '$1=="k"{print $2}' "$out/calibration/calibration.csv")"

# 2. Gamma grid: gamma = c / (sigma^2 T / 2), i.e. one lot of inventory moves the reservation
#    price by c ticks at mid-horizon, c = 0.01, 0.03, 0.1, 0.3, 1 (half-decade grid).
#    The stress test uses the middle value, c = 0.1. The symtv arms (A&S's time-varying spread, no
#    skew) are appended after the original list, and the skewc arms (A&S's skew, Symmetric's
#    constant spread; added after an independent review) after those, so the output of the arms
#    already there does not change.
read -r grid gmid ghi < <(python3 -c "
s, T = $sigma, 600.0
g = [c / (s * s * T / 2) for c in (0.01, 0.03, 0.1, 0.3, 1.0)]
print(','.join(f'as:{x:.6g},sym:{x:.6g}' for x in g) + ','
      + ','.join(f'symtv:{x:.6g}' for x in g) + ','
      + ','.join(f'skewc:{x:.6g}' for x in g), f'{g[2]:.6g}', f'{g[4]:.6g}')")

# 3. Uninformed baseline: gamma sweep against the symmetric benchmark and the naive quoter, plus the
#    symtv and skewc arms at each gamma (2 x 2 decomposition of A&S against Symmetric into skew and
#    spread term, both orders). Seed 202.
"$bin/backtest_run" --strategies "none,naive,$grid" --episodes $N --seed 202 --bootstrap $B \
  --as-sigma "$sigma" --as-k "$k" --out "$out/baseline"

# 4. Stress grid: informed Hawkes flow, news rate x branching ratio (plus no informed flow). Seed 303.
"$bin/stress_test" --strategies "none,naive,as:$gmid,sym:$gmid,symtv:$gmid,skewc:$gmid" \
  --nu-grid 0,0.01,0.03,0.1 --nbr-grid 0.5,0.75,0.9 --episodes $N --seed 303 --bootstrap $B \
  --as-sigma "$sigma" --as-k "$k" --out "$out/stress"

# 5. Monte Carlo error of the bootstrap endpoints: the
#    headline pairs recomputed with 10,000 replicates on the same episodes (same seeds, only the two
#    arms concerned): A&S against Symmetric at c = 1 in the baseline and at c = 0.1 in the stress
#    cell (0.1, 0.9).
"$bin/backtest_run" --strategies "as:$ghi,sym:$ghi" --episodes $N --seed 202 --bootstrap 10000 \
  --as-sigma "$sigma" --as-k "$k" --out "$out/mc_check/baseline_c1"
"$bin/backtest_run" --strategies "as:$gmid,sym:$gmid" --news-rate 0.1 --branching 0.9 \
  --episodes $N --seed 303 --bootstrap 10000 --as-sigma "$sigma" --as-k "$k" \
  --out "$out/mc_check/stress_0.1_0.9"

# 6. Sensitivity to the estimated k and sigma (not extended with skewc arms): the as, symtv and sym
#    arms get k x 0.5, k x 2,
#    sigma x 0.8 or sigma x 1.25 (one at a time; gamma unchanged), and the unperturbed values, on
#    the first NS episodes of the baseline (seed 202; c = 0.1 and c = 1) and of the stress cell
#    (0.1, 0.9) (seed 303; c = 0.1), so every setting sees the same episodes.
sens=()
for setting in "1 1" "0.5 1" "2 1" "1 0.8" "1 1.25"; do
  read -r ks ss <<< "$setting"
  tag="k${ks}_s${ss}"
  "$bin/backtest_run" --strategies "as:$gmid,symtv:$gmid,sym:$gmid,as:$ghi,symtv:$ghi,sym:$ghi" \
    --episodes $NS --seed 202 --bootstrap $B --as-sigma "$sigma" --as-k "$k" \
    --k-scale "$ks" --sigma-scale "$ss" --out "$out/sensitivity/uninformed_$tag"
  "$bin/backtest_run" --strategies "as:$gmid,symtv:$gmid,sym:$gmid" --news-rate 0.1 --branching 0.9 \
    --episodes $NS --seed 303 --bootstrap $B --as-sigma "$sigma" --as-k "$k" \
    --k-scale "$ks" --sigma-scale "$ss" --out "$out/sensitivity/stress_0.1_0.9_$tag"
  sens+=("sensitivity/uninformed_$tag" "sensitivity/stress_0.1_0.9_$tag")
done

{
  echo "generated: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "commit: $(git rev-parse HEAD 2>/dev/null || echo unknown)"
  echo "working tree before the run: $tree"
  echo "compiler: $(grep -m1 CMAKE_CXX_COMPILER: "$build/CMakeCache.txt" | cut -d= -f2)"
  echo "build type: $(grep -m1 CMAKE_BUILD_TYPE: "$build/CMakeCache.txt" | cut -d= -f2)"
  echo "python: $(python3 --version 2>&1); numpy $(python3 -c 'import numpy; print(numpy.__version__)'), pandas $(python3 -c 'import pandas; print(pandas.__version__)'), matplotlib $(python3 -c 'import matplotlib; print(matplotlib.__version__)')"
  echo "machine: $(nproc --all) cores, $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | sed 's/^ //')"
  echo "load average (1 min) at start: $load_start, at end: $(cut -d' ' -f1 /proc/loadavg)"
  echo "episodes per strategy: $N (calibration 1000, sensitivity $NS); bootstrap replicates: $B"
  echo "A&S estimates: sigma=$sigma ticks/sqrt(s), k=$k 1/tick"
  for d in calibration baseline stress mc_check/baseline_c1 mc_check/stress_0.1_0.9 "${sens[@]}"; do echo "--- $d"; cat "$out/$d/run.txt"; done
} > "$out/MANIFEST.txt"

python3 backtest/make_tables.py
python3 backtest/plot.py
