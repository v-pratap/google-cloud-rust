#!/usr/bin/env bash
# Interleaved A/B check for 100 MB w1r3: 4 rounds x 3 impls x 25 iterations,
# so all three implementations see the same network conditions over time.
set -uo pipefail
STANDARD="${1:?usage: $0 <standard-bucket>}"
ROOT="$HOME/gcs-poc"
OUT="$ROOT/results/ab100mb"
mkdir -p "$OUT"
cd "$OUT"
P="$ROOT/gcs-cpp-rust-poc"
declare -A BIN=([native_cpp]="$P/native/build/ssb_native_cpp" [rust_wrapped]="$P/build/ssb_rust_wrapped"
                [rust_pure]="$P/build/cargo/release/examples/pure_rust_bench")
date -u +start=%FT%TZ | tee run-info.txt
for round in 1 2 3 4; do
  for impl in native_cpp rust_wrapped rust_pure; do
    echo "round $round $impl"
    "${BIN[$impl]}" --bucket="$STANDARD" --w1r3-bucket="$STANDARD" --workload=w1r3 --iterations=25 \
      --object-sizes=100000000 --out="$impl-w1r3-100000000-r$round.csv" > "$impl-r$round.log" 2>&1
  done
done
date -u +end=%FT%TZ | tee -a run-info.txt
