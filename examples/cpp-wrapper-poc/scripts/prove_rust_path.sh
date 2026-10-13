#!/usr/bin/env bash
# Copyright 2026 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Proves that the C++ POC binary talks to GCS through the *Rust* SDK
# (google-cloud-storage crate) and contains no google-cloud-cpp code.
#
# Usage: scripts/prove_rust_path.sh <rapid-bucket> [--live]
#   --live  also attach gdb during a real read and print the stack (needs gdb)

set -euo pipefail
cd "$(dirname "$0")/.."

BUCKET="${1:?usage: $0 <rapid-bucket> [--live]}"
LIVE="${2:-}"
WRAPPED=build/quickstart
NATIVE=native/build/native_cpp_bench

hr() { printf '\n==== %s ====\n' "$1"; }

hr "1. What the C++ binary is LINKED against (from the build system)"
awk '/^build quickstart:/{p=1} p&&/LINK_LIBRARIES/{print; exit}' build/build.ninja
echo "-> only our C++ wrapper (libgcs_rust.a) + the Rust core (libgcs_core.a). No google-cloud-cpp, no gRPC C++."

hr "2. Shared libraries loaded at runtime (ldd)"
ldd "$WRAPPED"
echo "-> only libc/libstdc++/libm. Nothing from google-cloud-cpp, grpc, protobuf or abseil."

hr "3. Code actually INSIDE each binary (symbol counts)"
count() {
  nm -C "$1" | awk '
    /google::cloud::/         {cpp++}
    /grpc_core::/             {grpc++}
    /google_cloud_storage::/  {rs++}
    /tonic::/                 {tonic++}
    /tokio::/                 {tokio++}
    END {printf "  %-34s %7d\n  %-34s %7d\n  %-34s %7d\n  %-34s %7d\n  %-34s %7d\n",
         "google::cloud::  (C++ SDK)", cpp+0, "grpc_core::      (C++ gRPC)", grpc+0,
         "google_cloud_storage:: (Rust SDK)", rs+0, "tonic::          (Rust gRPC)", tonic+0,
         "tokio::          (Rust async)", tokio+0}'
}
echo "[$WRAPPED]  (the POC: C++ app -> Rust core)"
count "$WRAPPED"
if [[ -x "$NATIVE" ]]; then
  echo "[$NATIVE]  (control: plain google-cloud-cpp)"
  count "$NATIVE"
fi

hr "4. It really works against GCS"
"$WRAPPED" "$BUCKET" 1

if [[ "$LIVE" == "--live" ]]; then
  hr "5. LIVE: stack captured inside the Rust SDK while bytes arrive from GCS"
  # Address of google_cloud_storage::storage::bidi::active_read::ActiveRead::handle_data
  # (no `exit` in awk: that would SIGPIPE nm and trip `set -o pipefail`).
  OFF=$(nm "$WRAPPED" | awk '/ActiveRead.*handle_data/ && / T / && !f {print $1; f=1}')
  gdb -q -batch -ex "set pagination off" -ex "set print frame-arguments none" \
      -ex "starti" -ex "break *(0x555555554000+0x$OFF)" -ex "continue" \
      -ex "bt 8" -ex "kill" --args "$WRAPPED" "$BUCKET" 1 2>/dev/null |
    awk '/hit Breakpoint|^#/' | sed -E 's/<[^<>]{40,}>/<..>/g' | cut -c1-170 || true

  hr "6. LIVE: the same read, seen from the C++ side (C++ -> C ABI -> Rust)"
  gdb -q -batch -ex "set pagination off" -ex "set print frame-arguments none" \
      -ex "break ReadTrampoline" -ex "run" -ex "bt 30" -ex "kill" \
      --args "$WRAPPED" "$BUCKET" 1 2>/dev/null |
    awk '/^#/ && (/ReadTrampoline|gcs_core::gcs_object_read_range|gcs_core::ffi_call|Runtime::block_on|gcs_rust::|main/)' |
    sed -E 's/<[^<>]{40,}>/<..>/g' | cut -c1-170 || true
fi
