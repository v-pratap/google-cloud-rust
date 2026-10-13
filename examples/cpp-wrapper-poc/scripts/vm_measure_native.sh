#!/usr/bin/env bash
# Second pass of vm_measure_build.sh: native google-cloud-cpp builds from
# scratch (the vcpkg deps under $M/vcpkg-installed were already built from
# source by the first pass and are reused) + repository statistics.
#
# google-cloud-cpp always appends monitoring;trace;opentelemetry;universe_domain
# to GOOGLE_CLOUD_CPP_ENABLE, and monitoring/trace need gRPC. A storage-only
# (JSON/REST) build must disable them with a leading "-".
set -uo pipefail
ROOT="$HOME/gcs-poc"
M="$ROOT/measure"
OUT="$ROOT/results"
LOGS="$OUT/build-logs"
MET="$OUT/build-metrics-native.txt"
J="$(nproc)"
mkdir -p "$LOGS"; : > "$MET"
cd "$M"
kv() { echo "$1=$2" | tee -a "$MET"; }
bytes() { du -sb "$@" 2>/dev/null | awk '{s+=$1} END {printf "%.0f", s}'; }
t() {
  local key=$1; shift
  local s; s=$(date +%s.%N)
  if "$@" > "$LOGS/$key.log" 2>&1; then
    kv "time_s.$key" "$(awk -v a="$s" -v b="$(date +%s.%N)" 'BEGIN {printf "%.1f", b - a}')"
  else
    kv "time_s.$key" "FAILED"; tail -30 "$LOGS/$key.log"
  fi
}
kv cpus "$J"; kv date "$(date -u +%FT%TZ)"

# ---------------------------------------------------------------- repo stats
if [[ -z "${SKIP_REPO:-}" ]]; then
for r in google-cloud-cpp google-cloud-rust; do
  g=$(bytes "$r/.git"); a=$(bytes "$r")
  kv "repo.$r.git_bytes" "$g"
  kv "repo.$r.checkout_bytes" "$((a - g))"
  kv "repo.$r.files" "$(git -C $r ls-files | wc -l)"
  kv "repo.$r.commits" "$(git -C $r rev-list --count HEAD)"
  kv "repo.$r.head" "$(git -C $r describe --tags 2>/dev/null || git -C $r log --oneline -1)"
done
loc() { git -C "$1" ls-files -z -- "${@:2}" | (cd "$1" && xargs -0 cat) | wc -l; }
kv repo.google-cloud-cpp.storage_loc "$(loc google-cloud-cpp ':(glob)google/cloud/storage/**/*.cc' ':(glob)google/cloud/storage/**/*.h')"
kv repo.google-cloud-cpp.common_loc "$(loc google-cloud-cpp ':(glob)google/cloud/*.cc' ':(glob)google/cloud/*.h' ':(glob)google/cloud/internal/*.cc' ':(glob)google/cloud/internal/*.h')"
kv repo.google-cloud-rust.storage_loc "$(loc google-cloud-rust ':(glob)src/storage/**/*.rs')"
kv repo.google-cloud-rust.crates "$(git -C google-cloud-rust ls-files ':(glob)**/Cargo.toml' | wc -l)"
kv repo.google-cloud-cpp.libraries "$(ls -d google-cloud-cpp/google/cloud/*/ | wc -l)"
# What a user actually downloads: release source tarballs, shallow clones.
kv src.google-cloud-cpp_release_tgz_bytes "$(curl -sSfL https://github.com/googleapis/google-cloud-cpp/archive/refs/tags/v3.11.0.tar.gz | wc -c)"
kv src.google-cloud-rust_tgz_bytes "$(curl -sSfL https://github.com/googleapis/google-cloud-rust/archive/fe42ce042.tar.gz | wc -c)"
kv src.google-cloud-storage_crate_bytes "$(curl -sSfL https://static.crates.io/crates/google-cloud-storage/google-cloud-storage-1.20.0.crate | wc -c)"
rm -rf shallow && mkdir shallow
git clone -q --depth 1 --branch v3.11.0 https://github.com/googleapis/google-cloud-cpp.git shallow/cpp
git clone -q --depth 1 https://github.com/googleapis/google-cloud-rust.git shallow/rust
kv repo.google-cloud-cpp.shallow_clone_bytes "$(bytes shallow/cpp)"
kv repo.google-cloud-rust.shallow_clone_bytes "$(bytes shallow/rust)"
rm -rf shallow
fi

# ------------------------------------------------- native builds from scratch
VI="$M/vcpkg-installed"
# google-cloud-cpp >= 3.0 needs the opentelemetry-cpp API headers even for
# storage-only builds (google/cloud/internal/opentelemetry_context.h).
export VCPKG_BINARY_SOURCES=clear VCPKG_DOWNLOADS="$M/vcpkg-downloads"
t native.deps_otel "$M/vcpkg/vcpkg" install --x-install-root="$VI" opentelemetry-cpp
kv native.deps_json_otel_installed_bytes "$(du -sb "$VI" | awk '{printf "%.0f", $1}')"
gcc_build() {  # gcc_build <dir> <enable-list>
  rm -rf "$1" "$1-install"
  cmake -S "$M/google-cloud-cpp" -B "$1" -GNinja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$VI/x64-linux" "-DGOOGLE_CLOUD_CPP_ENABLE=$2" \
    -DBUILD_TESTING=OFF -DGOOGLE_CLOUD_CPP_WITH_MOCKS=OFF -DGOOGLE_CLOUD_CPP_ENABLE_EXAMPLES=OFF \
    -DGOOGLE_CLOUD_CPP_ENABLE_WERROR=OFF -DCMAKE_INSTALL_PREFIX="$1-install" &&
  cmake --build "$1" -j "$J" && cmake --install "$1"
}
NOEXTRA="-monitoring;-trace;-opentelemetry;-universe_domain"
t native.gcc_storage_json gcc_build "$M/gcc-json" "storage;$NOEXTRA"
kv native.gcc_json_build_dir_bytes "$(bytes "$M/gcc-json")"
kv native.gcc_json_install_bytes "$(bytes "$M/gcc-json-install")"
for a in storage rest_internal common; do
  kv "size.libgoogle_cloud_cpp_${a}_a" "$(stat -c %s "$M/gcc-json-install/lib/libgoogle_cloud_cpp_${a}.a" 2>/dev/null || echo 0)"
done
kv size.native_json_all_libs_a "$(find "$M/gcc-json-install/lib" "$VI/x64-linux/lib" -maxdepth 1 -name '*.a' \
  \( -name 'libgoogle_cloud_cpp_*' -o -name 'libabsl_*' -o -name 'libcrc32c*' -o -name 'libcurl*' -o -name 'libssl*' -o -name 'libcrypto*' -o -name 'libz*' \) -printf '%s\n' | awk '{s+=$1} END {printf "%d", s}')"

rm -rf "$M/json-native/build"
t native.json_min_app bash -c "cmake -S '$M/json-native' -B '$M/json-native/build' -GNinja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH='$M/gcc-json-install;$VI/x64-linux' && cmake --build '$M/json-native/build'"
B="$M/json-native/build/json_min_native"
kv size.json_min_native "$(stat -c %s "$B")"
cp "$B" /tmp/n && strip /tmp/n && kv size.json_min_native_stripped "$(stat -c %s /tmp/n)"

t native.gcc_storage_grpc gcc_build "$M/gcc-grpc" "storage;storage_grpc;$NOEXTRA"
kv size.libgoogle_cloud_cpp_storage_grpc_a "$(stat -c %s "$M/gcc-grpc-install/lib/libgoogle_cloud_cpp_storage_grpc.a" 2>/dev/null || echo 0)"
kv native.gcc_grpc_install_bytes "$(bytes "$M/gcc-grpc-install")"

touch "$M/google-cloud-cpp/google/cloud/storage/client.cc"
t native.incremental_rebuild cmake --build "$M/gcc-json" -j "$J"
# Same JSON-only library build limited to 8 cores (closer to a laptop / CI runner).
t native.gcc_storage_json_j8 bash -c "rm -rf '$M/gcc-j8' && cmake -S '$M/google-cloud-cpp' -B '$M/gcc-j8' -GNinja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH='$VI/x64-linux' '-DGOOGLE_CLOUD_CPP_ENABLE=storage;$NOEXTRA' -DBUILD_TESTING=OFF -DGOOGLE_CLOUD_CPP_WITH_MOCKS=OFF -DGOOGLE_CLOUD_CPP_ENABLE_EXAMPLES=OFF && cmake --build '$M/gcc-j8' -j 8"
export PATH="$M/rust/bin:$M/cargo-home/bin:$PATH" CARGO_HOME="$M/cargo-home"
t rust.cargo_build_release_j8 bash -c "cd '$M/poc/rust' && cargo build --release --lib -j 8 --target-dir '$M/cargo-j8'"
kv done "$(date -u +%FT%TZ)"
