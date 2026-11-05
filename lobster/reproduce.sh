#!/usr/bin/env bash
# Regenerates every number under results/lobster/ (about 5 minutes single-threaded).
# Usage: lobster/reproduce.sh [build-dir] [data-dir]
#   build-dir  default build-lobster; lobster_replay must already be built (Release)
#   data-dir   where the synthetic LOBSTER-layout files are written (default: a new temporary
#              directory). Each file is deleted after use; nothing is written into the repository
#              except results/lobster/.
set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="${1:-build-lobster}"
data="${2:-$(mktemp -d)}"
bin="$repo/$build/lobster/lobster_replay"
synth="lobster/tools/synth_lobster.py"
out="results/lobster"
cd "$repo"

[[ -x "$bin" ]] || { echo "missing $bin: build it first (see lobster/README.md)" >&2; exit 1; }
mkdir -p "$data"
# Remove only what this script writes (mutation_check.txt comes from tests/mutation_check.py).
rm -rf "$out/validation" "$out/cut" "$out/bench" "$out/MANIFEST.txt" "$out/tables.md"
mkdir -p "$out"
manifest="$out/MANIFEST.txt"
load() { cut -d' ' -f1 /proc/loadavg 2>/dev/null || echo n/a; }

{
  echo "generated: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "commit: $(git rev-parse HEAD 2>/dev/null || echo unknown)"
  if [[ -n "$(git status --porcelain -- lobster 2>/dev/null)" ]]; then
    echo "working tree: lobster/ has uncommitted changes relative to that commit"
  fi
  echo "compiler: $(grep -m1 CMAKE_CXX_COMPILER: "$build/CMakeCache.txt" | cut -d= -f2)"
  echo "build type: $(grep -m1 CMAKE_BUILD_TYPE: "$build/CMakeCache.txt" | cut -d= -f2)"
  echo "python: $(python3 --version 2>&1)"
  cpu="$(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2 | xargs || true)"
  echo "machine: $(nproc) cores, ${cpu:-unknown CPU}"
  echo "load average at start: $(load)"
  echo "data directory (synthetic files, deleted after use): $data"
  echo "--- commands (seeds are the --seed arguments)"
} > "$manifest"

run() {  # run COMMAND...: log it to the manifest, then run it
  echo "$*" >> "$manifest"
  "$@"
}

replay() {  # replay EXPECTED ARGS...: lobster_replay; EXPECTED is 0, 1 or "0|1" (not fatal)
  local want="$1"
  shift
  local rc=0
  echo "$bin $*  # expected exit code $want" >> "$manifest"
  "$bin" "$@" --quiet || rc=$?
  if [[ "|$want|" != *"|$rc|"* ]]; then
    echo "lobster_replay exited with $rc, expected $want: $*" >&2
    exit 1
  fi
}

# 1. Validation on synthetic files: strict policy, every row checked. Seeds fixed in advance.
for seed in 101 102; do
  for levels in 1 5 10; do
    name="seed${seed}_n${levels}"
    run python3 "$synth" --messages 500000 --levels "$levels" --seed "$seed" \
      --out "$data/$name" > "$data/$name.gen.txt"
    mkdir -p "$out/validation/$name"
    grep -v '_file=' "$data/$name.gen.txt" > "$out/validation/$name/generator.txt"
    replay 0 --messages "$data/${name}_message_${levels}.csv" \
      --book "$data/${name}_orderbook_${levels}.csv" --levels "$levels" --stats \
      --out "$out/validation/$name"
    rm -f "$data/${name}_message_${levels}.csv" "$data/${name}_orderbook_${levels}.csv"
  done
done

# 2. Window that starts mid-day (--cut): strict, lenient, and lenient with --bootstrap.
for levels in 1 5 10; do
  name="cut_seed201_n${levels}"
  run python3 "$synth" --messages 500000 --levels "$levels" --seed 201 --cut 100000 \
    --out "$data/$name" > "$data/$name.gen.txt"
  mkdir -p "$out/cut/$name"
  grep -v '_file=' "$data/$name.gen.txt" > "$out/cut/$name/generator.txt"
  m="$data/${name}_message_${levels}.csv"
  b="$data/${name}_orderbook_${levels}.csv"
  replay 1 --messages "$m" --book "$b" --out "$out/cut/$name/strict"
  replay "0|1" --messages "$m" --book "$b" --policy lenient --out "$out/cut/$name/lenient"
  replay "0|1" --messages "$m" --book "$b" --policy lenient --bootstrap \
    --out "$out/cut/$name/lenient_bootstrap"
  rm -f "$m" "$b"
done

# 3. Throughput on a 2,000,000-message file (N = 10), 7 passes per stage.
run python3 "$synth" --messages 2000000 --levels 10 --seed 301 --out "$data/bench" \
  > "$data/bench.gen.txt"
mkdir -p "$out/bench"
grep -v '_file=' "$data/bench.gen.txt" > "$out/bench/generator.txt"
# Timing is noisy on a shared machine: wait (at most LOBSTER_BENCH_WAIT seconds, default 600) for
# the 1-minute load average to drop below 1.3, then run anyway; the load is recorded either way.
waited=0
limit="${LOBSTER_BENCH_WAIT:-600}"
while awk -v l="$(load)" 'BEGIN { exit !(l + 0 >= 1.3) }' && ((waited < limit)); do
  sleep 15
  waited=$((waited + 15))
done
echo "waited ${waited} s for a quiet machine (limit ${limit} s)" >> "$manifest"
echo "load average before the benchmark: $(load)" >> "$manifest"
replay 0 --messages "$data/bench_message_10.csv" --book "$data/bench_orderbook_10.csv" \
  --bench --bench-passes 7 --out "$out/bench"
echo "load average after the benchmark: $(load)" >> "$manifest"
for f in "$data/bench_message_10.csv" "$data/bench_orderbook_10.csv"; do
  echo "file size: $(wc -c < "$f") bytes, $(basename "$f")" >> "$manifest"
done
rm -f "$data/bench_message_10.csv" "$data/bench_orderbook_10.csv"

run python3 lobster/tools/make_tables.py
echo "done: $out"
