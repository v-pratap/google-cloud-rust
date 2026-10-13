#!/usr/bin/env bash
# GCE startup script for the GCS C++/Rust POC benchmark (no SSH needed).
#
# Pulls sources from gs://$ART/src, installs the toolchain, builds both SSB
# drivers, runs 4 workloads x $ITER iterations for rust_wrapped / native_cpp /
# native_cpp-dp, and uploads results + logs to gs://$ART/results/<host>/.
# Progress is printed with a "POC:" prefix so it can be followed on the serial
# console:  gcloud compute instances get-serial-port-output ... | grep POC:
#
# Idempotent: each phase writes a marker under $ROOT/.done-* and is skipped on
# reboot.
set -uo pipefail

MD=http://metadata.google.internal/computeMetadata/v1/instance/attributes
meta() { curl -sf -H Metadata-Flavor:Google "$MD/$1"; }
ART="$(meta poc-artifacts-bucket || echo YOUR_ARTIFACTS_BUCKET)"
BUCKET="$(meta poc-bench-bucket || echo YOUR_RAPID_BUCKET)"
W1R3_BUCKET="$(meta poc-w1r3-bucket || echo YOUR_STANDARD_BUCKET)"
ITER="$(meta poc-iterations || echo 200)"
RUN_ID="$(meta poc-run-id || echo run1)"

export HOME=/root
ROOT=/opt/gcs-poc
mkdir -p "$ROOT"
cd "$ROOT"
LOG="$ROOT/startup.log"
exec > >(tee -a "$LOG") 2>&1
HOST="$(hostname)"
DEST="gs://$ART/results/$HOST/$RUN_ID"

# New run id => refetch sources, rebuild, re-run (toolchain + vcpkg are reused).
if [[ "$(cat "$ROOT/.run-id" 2>/dev/null)" != "$RUN_ID" ]]; then
  rm -f "$ROOT"/.done-{fetch_src,setup,build,smoke,bench,upload} "$ROOT/DONE"
  [[ -d "$ROOT/results" ]] && mv "$ROOT/results" "$ROOT/results.$(date +%s)"
  echo "$RUN_ID" > "$ROOT/.run-id"
fi

say() { echo "POC: $(date -u +%T) $*"; }
sync_logs() { gcloud storage cp -q "$ROOT"/*.log "$DEST/logs/" >/dev/null 2>&1 || true; }
phase() {  # phase <name> <command...>
  local name=$1; shift
  if [[ -f "$ROOT/.done-$name" ]]; then say "skip $name (done)"; return 0; fi
  say "BEGIN $name"
  if "$@" > "$ROOT/$name.log" 2>&1; then
    touch "$ROOT/.done-$name"; say "OK $name"; sync_logs
  else
    say "FAILED $name (exit $?) -- last lines:"; tail -25 "$ROOT/$name.log" | sed 's/^/POC:   /'
    sync_logs; exit 1
  fi
}

say "start run=$RUN_ID art=$ART rapid=$BUCKET standard=$W1R3_BUCKET iterations=$ITER"

packages() {
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -q
  apt-get install -y -q build-essential cmake ninja-build git pkg-config curl zip unzip \
    tar gpg xz-utils python3 autoconf automake libtool bison flex
}

fetch_src() {
  # vm_setup.sh clones the fork itself; only the setup script is needed here.
  local ref="${POC_RUST_REF:-poc/cpp-rust-wrapper}"
  curl -fsSL -o "$ROOT/vm_setup.sh" \
    "https://raw.githubusercontent.com/v-pratap/google-cloud-rust/$ref/examples/cpp-wrapper-poc/scripts/vm_setup.sh"
  chmod +x "$ROOT/vm_setup.sh"
}

vcpkg_setup() {
  [[ -d "$HOME/vcpkg" ]] || git clone -q https://github.com/microsoft/vcpkg.git "$HOME/vcpkg"
  "$HOME/vcpkg/bootstrap-vcpkg.sh" -disableMetrics
}

setup() {
  # vm_setup.sh uses ~/gcs-poc; point it at $ROOT.
  ln -sfn "$ROOT" "$HOME/gcs-poc"
  "$ROOT/vm_setup.sh"
}

build() {
  ln -sfn "$ROOT" "$HOME/gcs-poc"
  "$ROOT/gcs-cpp-rust-poc/scripts/vm_build.sh"
}

smoke() {
  "$ROOT/gcs-cpp-rust-poc/build/quickstart" "$BUCKET" 8
}

bench() {
  local mode; mode="$(meta poc-mode || echo ssb)"
  if [[ "$mode" == "verify" ]]; then
    "$ROOT/gcs-cpp-rust-poc/scripts/vm_run_verify.sh" "$BUCKET" "$ITER"
  elif [[ "$mode" == "ab100mb" ]]; then
    "$ROOT/gcs-cpp-rust-poc/scripts/vm_run_ab100mb.sh" "$W1R3_BUCKET"
  elif [[ "$mode" == "measure-native" ]]; then
    SKIP_REPO=1 "$ROOT/gcs-cpp-rust-poc/scripts/vm_measure_native.sh" || true
    "$ROOT/gcs-cpp-rust-poc/scripts/vm_run_ab100mb.sh" "$W1R3_BUCKET"
  elif [[ "$mode" == "w1r3" ]]; then
    "$ROOT/gcs-cpp-rust-poc/scripts/vm_run_w1r3.sh" "$W1R3_BUCKET" "$ITER"
    gcloud storage cp -q -r "$ROOT/results" "$DEST/" || true   # keep bench data safe
    "$ROOT/gcs-cpp-rust-poc/scripts/vm_measure_build.sh" || true
  else
    "$ROOT/gcs-cpp-rust-poc/scripts/vm_run_ssb.sh" "$BUCKET" "$W1R3_BUCKET" "$ITER"
  fi
}

upload_results() {
  gcloud storage cp -q -r "$ROOT/results" "$DEST/"
  echo done > "$ROOT/DONE"
  gcloud storage cp -q "$ROOT/DONE" "$DEST/DONE"
}

# Stream bench progress (per-workload lines) to the serial console while it runs.
progress_tail() {
  while sleep 60; do
    for f in "$ROOT"/results/*.log; do
      [[ -f $f ]] && say "progress $(basename "$f"): $(awk '/iterations in| ERROR /' "$f" | tail -1)"
    done
    gcloud storage cp -q "$ROOT"/results/*.csv "$DEST/partial/" >/dev/null 2>&1 || true
    sync_logs
  done
}

phase packages packages
phase fetch_src fetch_src
phase vcpkg vcpkg_setup
phase setup setup
phase build build
phase smoke smoke
progress_tail & TAIL=$!
phase bench bench
kill $TAIL 2>/dev/null
phase upload upload_results
say "ALL DONE -> $DEST"
