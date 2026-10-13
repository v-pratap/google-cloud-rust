#!/usr/bin/env bash
# Builds both SSB drivers on the VM (run after vm_setup.sh).
set -euo pipefail
ROOT="$HOME/gcs-poc"
export PATH="$HOME/.local/rust/bin:$HOME/.cargo/bin:$PATH"
cd "$ROOT/gcs-cpp-rust-poc"

echo "== wrapped (cargo + cbindgen + C++)"
cmake -S . -B build -GNinja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target ssb_rust_wrapped quickstart

echo "== pure Rust baseline (same crate, no wrapper)"
(cd rust && cargo build --release --example pure_rust_bench --target-dir ../build/cargo)

echo "== native google-cloud-cpp (storage_grpc only, vcpkg deps)"
cmake -S native -B native/build -GNinja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$HOME/vcpkg/scripts/buildsystems/vcpkg.cmake" \
  -DGCS_POC_GOOGLE_CLOUD_CPP_DIR="$ROOT/google-cloud-cpp"
cmake --build native/build --target ssb_native_cpp

ls -la build/ssb_rust_wrapped native/build/ssb_native_cpp
echo "build done"
