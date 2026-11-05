#!/usr/bin/env bash
# Regenerates every number and figure under results/paper/ (about 30 s single-threaded).
# Usage: paper/reproduce.sh [build-dir]   (default build-paper; the binary must already be built)
set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo"  # every path below is relative to the repository root, so none is recorded in results/
build="${1:-build-paper}"
bin="$build/paper/paper_replication"
out="results/paper"

[[ -x "$bin" ]] || { echo "missing $bin: build it first (see paper/README.md)" >&2; exit 1; }
mkdir -p "$out"

# Three independent seeds, one per experiment, fixed in advance (not tuned).
"$bin" --sims 1000 --seed 1 --bootstrap 2000 --trace 0 --out results/paper/sims1000
"$bin" --sims 100000 --seed 2 --per-sim 0 --bootstrap 1000 --block-size 1000 \
  --out results/paper/sims100000
gammas="$(python3 -c "import numpy as np; print(','.join(f'{g:.4g}' for g in np.logspace(-2, 0, 21)))")"
"$bin" --gamma "$gammas" --sims 10000 --seed 3 --per-sim 0 --bootstrap 500 \
  --out results/paper/sweep

{
  echo "generated: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "commit: $(git rev-parse HEAD 2>/dev/null || echo unknown)"
  if [[ -n "$(git status --porcelain -- paper 2>/dev/null)" ]]; then
    echo "working tree: paper/ has uncommitted changes relative to that commit"
  fi
  echo "compiler: $(grep -m1 CMAKE_CXX_COMPILER: "$build/CMakeCache.txt" | cut -d= -f2)"
  echo "build type: $(grep -m1 CMAKE_BUILD_TYPE: "$build/CMakeCache.txt" | cut -d= -f2)"
  for d in sims1000 sims100000 sweep; do echo "--- $d"; cat "results/paper/$d/run.txt"; done
} > "$out/MANIFEST.txt"

python3 paper/make_tables.py
python3 paper/plot.py
