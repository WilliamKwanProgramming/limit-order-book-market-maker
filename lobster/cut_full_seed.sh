#!/usr/bin/env bash
# The cut experiment with the bootstrap seeded from the complete book (see lobster/README.md,
# "Cut files: does the complete book remove the remaining mismatches?"). Writes
# results/lobster/cut_full_seed/ (about 3 minutes single-threaded).
# Usage: lobster/cut_full_seed.sh [build-dir] [data-dir]
#   build-dir  default build-lobster; lobster_replay and lobster_cut_full_seed must be built
#   data-dir   scratch directory for the synthetic files (default data/tmp/lobster, git-ignored);
#              each file is deleted after use.
# Same seed, message count and cut as the cut runs of lobster/reproduce.sh (seed 201, 500000
# messages after a cut of 100000), for N = 5 and N = 10. For each N two runs on the same files:
#   lenient_bootstrap   lobster_replay --policy lenient --bootstrap (ghosts from the N-level row 1)
#   lenient_full_seed   lobster_cut_full_seed (ghosts from the complete book at the cut)
set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo"
build="${1:-build-lobster}"
data="${2:-data/tmp/lobster}"
replay_bin="$build/lobster/lobster_replay"
full_bin="$build/lobster/lobster_cut_full_seed"
synth="lobster/tools/synth_lobster.py"
out="results/lobster/cut_full_seed"

for b in "$replay_bin" "$full_bin"; do
  [[ -x "$b" ]] || { echo "missing $b: build it first (see lobster/README.md)" >&2; exit 1; }
done
# Recorded before anything is written (this script's own outputs would make the tree dirty).
if [[ -n "$(git status --porcelain 2>/dev/null)" ]]; then
  tree="has uncommitted changes relative to that commit"
else
  tree="clean"
fi
mkdir -p "$data"
rm -rf "$out"
mkdir -p "$out"
manifest="$out/MANIFEST.txt"
{
  echo "generated: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "commit: $(git rev-parse HEAD 2>/dev/null || echo unknown)"
  echo "working tree before the run: $tree"
  echo "compiler: $(grep -m1 CMAKE_CXX_COMPILER: "$build/CMakeCache.txt" | cut -d= -f2)"
  echo "build type: $(grep -m1 CMAKE_BUILD_TYPE: "$build/CMakeCache.txt" | cut -d= -f2)"
  echo "python: $(python3 --version 2>&1)"
  echo "data directory (synthetic files, deleted after use): $data"
  echo "--- commands (seeds are the --seed arguments; exit code 1 means some row mismatched)"
} > "$manifest"

for levels in 5 10; do
  name="cut_seed201_n${levels}"
  full="$data/${name}_full_row.csv"
  cmd=(python3 "$synth" --messages 500000 --levels "$levels" --seed 201 --cut 100000
       --out "$data/$name" --full-row "$full")
  echo "${cmd[*]}" >> "$manifest"
  "${cmd[@]}" > "$data/$name.gen.txt"
  mkdir -p "$out/$name"
  grep -v '_file=' "$data/$name.gen.txt" > "$out/$name/generator.txt"
  m="$data/${name}_message_${levels}.csv"
  b="$data/${name}_orderbook_${levels}.csv"
  for run in lenient_bootstrap lenient_full_seed; do
    if [[ "$run" == lenient_bootstrap ]]; then
      cmd=("$replay_bin" --messages "$m" --book "$b" --policy lenient --bootstrap
           --out "$out/$name/$run" --quiet)
    else
      cmd=("$full_bin" --messages "$m" --book "$b" --full-row "$full" --out "$out/$name/$run")
    fi
    rc=0
    "${cmd[@]}" > /dev/null || rc=$?
    echo "${cmd[*]}  # exit code $rc" >> "$manifest"
    if ((rc > 1)); then
      echo "failed with exit code $rc: ${cmd[*]}" >&2
      exit 1
    fi
  done
  rm -f "$m" "$b" "$full" "$data/$name.gen.txt"
done

python3 lobster/tools/make_tables.py
echo "done: $out"
