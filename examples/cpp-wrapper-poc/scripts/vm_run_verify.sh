#!/usr/bin/env bash
# Verification run: native C++ vs Rust-wrapped C++ vs pure Rust (same crate,
# no wrapper), RAPID bucket, 200 iterations. open/random8k run twice in
# opposite order (A B C, then C B A) to detect drift over time.
set -uo pipefail
RAPID="${1:?usage: $0 <rapid-bucket> [iterations=200]}"
ITER="${2:-200}"
ROOT="$HOME/gcs-poc"
OUT="$ROOT/results"
mkdir -p "$OUT"
cd "$OUT"
W="$ROOT/gcs-cpp-rust-poc/build/ssb_rust_wrapped"
N="$ROOT/gcs-cpp-rust-poc/native/build/ssb_native_cpp"
P="$ROOT/gcs-cpp-rust-poc/build/cargo/release/examples/pure_rust_bench"
md() { curl -s -H Metadata-Flavor:Google "http://metadata/computeMetadata/v1/instance/$1" | awk -F/ '{print $NF}'; }
{
  echo "host=$(hostname) zone=$(md zone) machine=$(md machine-type) cpus=$(nproc)"
  echo "rapid_bucket=$RAPID iterations=$ITER start=$(date -u +%FT%TZ)"
  echo "google-cloud-rust=$(git -C "$ROOT/google-cloud-rust" log --oneline -1)"
  echo "google-cloud-cpp=$(git -C "$ROOT/google-cloud-cpp" describe --tags 2>/dev/null)"
} | tee run-info.txt

run() {  # run <impl> <workload> <pass>
  local impl=$1 w=$2 pass=$3 bin
  case $impl in
    native_cpp) bin=$N ;; rust_wrapped) bin=$W ;; rust_pure) bin=$P ;;
  esac
  echo "######## $w pass$pass: $impl  $(date -u +%T)"
  if [[ $impl == rust_pure ]]; then
    "$bin" --bucket="$RAPID" --workload="$w" --iterations="$ITER" --out="$impl-$w-pass$pass.csv" 2>&1 | tee "$impl-$w-pass$pass.log"
  else
    "$bin" --bucket="$RAPID" --workload="$w" --iterations="$ITER" --out="$impl-$w-pass$pass.csv" 2>&1 | tee "$impl-$w-pass$pass.log"
  fi
}

for w in open random8k; do
  for impl in native_cpp rust_wrapped rust_pure; do run $impl $w 1; done
  for impl in rust_pure rust_wrapped native_cpp; do run $impl $w 2; done
done
for impl in native_cpp rust_wrapped rust_pure; do run $impl read50x2x4 1; done
echo "end=$(date -u +%FT%TZ)" | tee -a run-info.txt
echo "ALL DONE"
