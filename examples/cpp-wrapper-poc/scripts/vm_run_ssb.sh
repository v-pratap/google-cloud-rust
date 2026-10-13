#!/usr/bin/env bash
# Runs the 4 SSB workloads x N iterations for each implementation, sequentially
# (never concurrently, so they don't compete for NIC/CPU). Results: ~/gcs-poc/results/
#   open / random8k / read50x2x4 -> RAPID bucket   (native: AsyncClient GRPC+DP)
#   w1r3                         -> STANDARD bucket (native: sync Client, JSON)
set -uo pipefail
RAPID="${1:?usage: $0 <rapid-bucket> <standard-bucket> [iterations=200]}"
STANDARD="${2:?usage: $0 <rapid-bucket> <standard-bucket> [iterations=200]}"
ITER="${3:-200}"
ROOT="$HOME/gcs-poc"
OUT="$ROOT/results"
mkdir -p "$OUT"
cd "$OUT"
W="$ROOT/gcs-cpp-rust-poc/build/ssb_rust_wrapped"
N="$ROOT/gcs-cpp-rust-poc/native/build/ssb_native_cpp"
md() { curl -s -H Metadata-Flavor:Google "http://metadata/computeMetadata/v1/instance/$1" | awk -F/ '{print $NF}'; }

{
  echo "host=$(hostname) zone=$(md zone) machine=$(md machine-type) cpus=$(nproc)"
  echo "rapid_bucket=$RAPID standard_bucket=$STANDARD iterations=$ITER start=$(date -u +%FT%TZ)"
  echo "google-cloud-rust=$(git -C "$ROOT/google-cloud-rust" log --oneline -1)"
  echo "google-cloud-cpp=$(git -C "$ROOT/google-cloud-cpp" describe --tags 2>/dev/null || git -C "$ROOT/google-cloud-cpp" log --oneline -1)"
} | tee run-info.txt

for w in w1r3 open random8k read50x2x4; do
  for impl in rust_wrapped native_cpp; do
    bin=$W; [[ $impl == native_cpp ]] && bin=$N
    echo "######## $w: $impl"
    "$bin" --bucket="$RAPID" --w1r3-bucket="$STANDARD" --workload="$w" --iterations="$ITER" \
      --out="$impl-$w.csv" 2>&1 | tee "$impl-$w.log"
  done
done
echo "end=$(date -u +%FT%TZ)" | tee -a run-info.txt
echo "ALL DONE"
