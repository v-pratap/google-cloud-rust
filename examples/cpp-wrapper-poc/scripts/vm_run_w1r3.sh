#!/usr/bin/env bash
# w1r3 (JSON, STANDARD bucket) only: 200 iterations PER object size for
# native_cpp, rust_wrapped and rust_pure. Runs are sequential (never two at a
# time) and the implementation order rotates per size to avoid ordering bias.
# Results: ~/gcs-poc/results/<impl>-w1r3-<size>.csv/.log
set -uo pipefail
STANDARD="${1:?usage: $0 <standard-bucket> [iterations=200]}"
ITER="${2:-200}"
ROOT="$HOME/gcs-poc"
OUT="$ROOT/results"
mkdir -p "$OUT"
cd "$OUT"
P="$ROOT/gcs-cpp-rust-poc"
W="$P/build/ssb_rust_wrapped"
N="$P/native/build/ssb_native_cpp"
R="$P/build/cargo/release/examples/pure_rust_bench"
md() { curl -s -H Metadata-Flavor:Google "http://metadata/computeMetadata/v1/instance/$1" | awk -F/ '{print $NF}'; }

{
  echo "host=$(hostname) zone=$(md zone) machine=$(md machine-type) cpus=$(nproc)"
  echo "standard_bucket=$STANDARD iterations_per_size=$ITER start=$(date -u +%FT%TZ)"
  echo "google-cloud-rust=$(git -C "$ROOT/google-cloud-rust" log --oneline -1)"
  echo "google-cloud-cpp=$(git -C "$ROOT/google-cloud-cpp" describe --tags 2>/dev/null || git -C "$ROOT/google-cloud-cpp" log --oneline -1)"
  echo "kernel=$(uname -r) glibc=$(ldd --version | head -1 | awk '{print $NF}')"
} | tee run-info.txt

run() {  # run <impl> <size>
  local impl=$1 size=$2 out="$1-w1r3-$2"
  echo "######## w1r3 size=$size: $impl"
  case $impl in
    native_cpp)   "$N" --bucket="$STANDARD" --w1r3-bucket="$STANDARD" --workload=w1r3 \
                    --iterations="$ITER" --object-sizes="$size" --out="$out.csv" ;;
    rust_wrapped) "$W" --bucket="$STANDARD" --w1r3-bucket="$STANDARD" --workload=w1r3 \
                    --iterations="$ITER" --object-sizes="$size" --out="$out.csv" ;;
    rust_pure)    "$R" --bucket="$STANDARD" --w1r3-bucket="$STANDARD" --workload=w1r3 \
                    --iterations="$ITER" --object-sizes="$size" --out="$out.csv" ;;
  esac > "$out.log" 2>&1
  echo "  exit=$? $(awk '/iterations in/' "$out.log" | tail -1)"
}

orders=("native_cpp rust_wrapped rust_pure" "rust_wrapped rust_pure native_cpp" "rust_pure native_cpp rust_wrapped")
k=0
for size in 100000 2097152 100000000; do
  for impl in ${orders[$((k % 3))]}; do run "$impl" "$size"; done
  k=$((k + 1))
done
echo "end=$(date -u +%FT%TZ)" | tee -a run-info.txt
