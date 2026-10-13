#!/usr/bin/env bash
# From-scratch build, size and toolchain measurements (run AFTER the
# benchmarks so nothing competes for CPU). Everything is built in a fresh
# directory with empty caches:
#   * google-cloud-cpp storage (JSON only) + its vcpkg deps, from source
#   * google-cloud-cpp storage + storage_grpc (adds gRPC/protobuf), from source
#   * Rust toolchain, cbindgen, crates download, Rust core, C++ wrapper
# Output: ~/gcs-poc/results/build-metrics.txt (key=value) + logs in results/build-logs/.
set -uo pipefail
ROOT="$HOME/gcs-poc"
M="$ROOT/measure"
OUT="$ROOT/results"
LOGS="$OUT/build-logs"
MET="$OUT/build-metrics.txt"
CPP_TAG="${POC_CPP_TAG:-v3.11.0}"
RUST_COMMIT="${POC_RUST_COMMIT:-fe42ce042}"
RUST_VERSION=1.99.0
J="$(nproc)"
rm -rf "$M"; mkdir -p "$M" "$LOGS"; : > "$MET"
cd "$M"

kv() { echo "$1=$2" | tee -a "$MET"; }
bytes() { du -sb "$@" 2>/dev/null | awk '{s+=$1} END {printf "%d", s}'; }
# t <key> <cmd...> : run, log to $LOGS/<key>.log, record wall seconds.
t() {
  local key=$1; shift
  local s; s=$(date +%s.%N)
  if "$@" > "$LOGS/$key.log" 2>&1; then
    kv "time_s.$key" "$(awk -v a="$s" -v b="$(date +%s.%N)" 'BEGIN {printf "%.1f", b - a}')"
  else
    kv "time_s.$key" "FAILED"; tail -20 "$LOGS/$key.log"
  fi
}

kv host "$(hostname)"; kv cpus "$J"; kv date "$(date -u +%FT%TZ)"
kv gcc "$(gcc -dumpfullversion)"; kv cmake "$(cmake --version | head -1 | awk '{print $3}')"

# ---------------------------------------------------------------- 1. repos
t clone.google-cloud-cpp git clone -q https://github.com/googleapis/google-cloud-cpp.git
t clone.google-cloud-rust git clone -q https://github.com/googleapis/google-cloud-rust.git
git -C google-cloud-cpp checkout -q "$CPP_TAG"
# The fork has the same history plus the POC branch; the commit is upstream.
git -C google-cloud-rust checkout -q "$RUST_COMMIT" || {
  git -C google-cloud-rust remote add fork https://github.com/v-pratap/google-cloud-rust.git
  git -C google-cloud-rust fetch -q fork && git -C google-cloud-rust checkout -q "$RUST_COMMIT"; }
for r in google-cloud-cpp google-cloud-rust; do
  kv "repo.$r.git_bytes" "$(bytes $r/.git)"
  kv "repo.$r.checkout_bytes" "$(( $(bytes $r) - $(bytes $r/.git) ))"
  kv "repo.$r.files" "$(git -C $r ls-files | wc -l)"
  kv "repo.$r.commits" "$(git -C $r rev-list --count HEAD)"
done
kv repo.google-cloud-cpp.storage_bytes "$(bytes google-cloud-cpp/google/cloud/storage)"
kv repo.google-cloud-cpp.storage_files "$(git -C google-cloud-cpp ls-files google/cloud/storage | wc -l)"
kv repo.google-cloud-cpp.storage_loc "$(git -C google-cloud-cpp ls-files 'google/cloud/storage/*.cc' 'google/cloud/storage/*.h' | (cd google-cloud-cpp && xargs cat) | wc -l)"
kv repo.google-cloud-cpp.common_loc "$(git -C google-cloud-cpp ls-files 'google/cloud/*.cc' 'google/cloud/*.h' 'google/cloud/internal/*.cc' 'google/cloud/internal/*.h' | (cd google-cloud-cpp && xargs cat) | wc -l)"
kv repo.google-cloud-rust.storage_bytes "$(bytes google-cloud-rust/src/storage)"
kv repo.google-cloud-rust.storage_files "$(git -C google-cloud-rust ls-files src/storage | wc -l)"
kv repo.google-cloud-rust.storage_loc "$(git -C google-cloud-rust ls-files 'src/storage/*.rs' | (cd google-cloud-rust && xargs cat) | wc -l)"
kv repo.google-cloud-rust.workspace_members "$(awk '/^members/,/^\]/' google-cloud-rust/Cargo.toml | grep -c '"')"

# POC sources (wrapper), next to the two repos so relative paths resolve.
mkdir -p poc && (cd "$ROOT/gcs-cpp-rust-poc" && tar cf - CMakeLists.txt cpp rust native) | tar xf - -C poc
rm -rf poc/build
kv poc.cabi_rust_loc "$(cat poc/rust/src/lib.rs | wc -l)"
kv poc.cpp_wrapper_loc "$(cat poc/cpp/src/client.cc poc/cpp/include/gcs_rust/client.h | wc -l)"

# ------------------------------------------------------- 2. Rust toolchain
export CARGO_HOME="$M/cargo-home" RUSTUP_HOME="$M/rustup-home"
mkdir -p "$CARGO_HOME"
rust_install() {
  local T="rust-$RUST_VERSION-x86_64-unknown-linux-gnu"
  curl -sSfLO "https://static.rust-lang.org/dist/$T.tar.xz"
  curl -sSfLO "https://static.rust-lang.org/dist/$T.tar.xz.sha256"
  sha256sum -c "$T.tar.xz.sha256"
  echo "download_bytes $(stat -c %s $T.tar.xz)"
  tar xf "$T.tar.xz"
  "./$T/install.sh" --prefix="$M/rust" --disable-ldconfig \
    --components=rustc,cargo,rust-std-x86_64-unknown-linux-gnu
  rm -rf "$T" "$T.tar.xz"*
}
t toolchain.rust_install rust_install
kv toolchain.rust_download_bytes "$(awk '/download_bytes/ {print $2}' "$LOGS/toolchain.rust_install.log")"
kv toolchain.rust_installed_bytes "$(bytes "$M/rust")"
export PATH="$M/rust/bin:$CARGO_HOME/bin:$PATH"
kv toolchain.rustc "$(rustc --version)"
t toolchain.cbindgen_install cargo install -q cbindgen --locked --version 0.29.2
kv toolchain.cbindgen_installed_bytes "$(bytes "$CARGO_HOME/bin/cbindgen")"
rm -rf "$CARGO_HOME/registry"   # the crate build below starts with an empty cache

# ------------------------------------------------- 3. Rust core + wrapper
cd "$M/poc/rust"
t rust.cargo_fetch cargo fetch
kv rust.lock_packages "$(grep -c '^\[\[package\]\]' Cargo.lock)"
kv rust.registry_bytes "$(bytes "$CARGO_HOME/registry")"
t rust.cargo_build_release cargo build --release --lib --target-dir "$M/poc/build/cargo"
kv rust.target_dir_bytes "$(bytes "$M/poc/build/cargo")"
t rust.cargo_vendor cargo vendor -q "$M/vendor"
kv rust.vendor_bytes "$(bytes "$M/vendor")"
kv rust.vendor_tgz_bytes "$(tar czf - -C "$M" vendor | wc -c)"
cd "$M/poc"
t wrapper.cmake_configure cmake -S . -B build -GNinja -DCMAKE_BUILD_TYPE=Release
t wrapper.cmake_build cmake --build build --target json_min_wrapped quickstart -j "$J"
L=build/cargo/release
kv size.libgcs_core_a "$(stat -c %s $L/libgcs_core.a)"
cp $L/libgcs_core.a /tmp/c.a && strip --strip-debug /tmp/c.a && kv size.libgcs_core_a_nodebug "$(stat -c %s /tmp/c.a)"
kv size.libgcs_core_so "$(stat -c %s $L/libgcs_core.so)"
cp $L/libgcs_core.so /tmp/c.so && strip /tmp/c.so && kv size.libgcs_core_so_stripped "$(stat -c %s /tmp/c.so)"
for b in json_min_wrapped quickstart; do
  kv "size.$b" "$(stat -c %s build/$b)"
  cp build/$b /tmp/$b && strip /tmp/$b && kv "size.${b}_stripped" "$(stat -c %s /tmp/$b)"
done
kv size.libgcs_rust_a "$(stat -c %s build/libgcs_rust.a)"

# ------------------------------------- 4. native google-cloud-cpp (vcpkg)
cd "$M"
export VCPKG_BINARY_SOURCES=clear VCPKG_DOWNLOADS="$M/vcpkg-downloads"
mkdir -p "$VCPKG_DOWNLOADS"
t vcpkg.clone_bootstrap bash -c "git clone -q https://github.com/microsoft/vcpkg.git && ./vcpkg/bootstrap-vcpkg.sh -disableMetrics"
kv vcpkg.commit "$(git -C vcpkg log --oneline -1)"
VI="$M/vcpkg-installed"
t native.deps_json ./vcpkg/vcpkg install --x-install-root="$VI" abseil crc32c "curl[ssl]" nlohmann-json openssl
kv native.deps_json_installed_bytes "$(bytes "$VI")"

gcc_build() {  # gcc_build <dir> <enable-list>
  cmake -S "$M/google-cloud-cpp" -B "$1" -GNinja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$VI/x64-linux" "-DGOOGLE_CLOUD_CPP_ENABLE=$2" \
    -DBUILD_TESTING=OFF -DGOOGLE_CLOUD_CPP_WITH_MOCKS=OFF -DGOOGLE_CLOUD_CPP_ENABLE_EXAMPLES=OFF \
    -DGOOGLE_CLOUD_CPP_ENABLE_WERROR=OFF -DCMAKE_INSTALL_PREFIX="$1-install" &&
  cmake --build "$1" -j "$J" && cmake --install "$1"
}
t native.gcc_storage_json gcc_build "$M/gcc-json" "storage;-monitoring;-trace;-opentelemetry;-universe_domain"
kv native.gcc_json_install_bytes "$(bytes "$M/gcc-json-install")"
for a in storage rest_internal common; do
  kv "size.libgoogle_cloud_cpp_${a}_a" "$(stat -c %s "$M/gcc-json-install/lib/libgoogle_cloud_cpp_${a}.a" 2>/dev/null || echo 0)"
done

# Minimal native JSON app, linked against the install above.
mkdir -p "$M/json-native"
cp "$M/poc/cpp/examples/json_min_native.cc" "$M/json-native/"
cat > "$M/json-native/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.16)
project(json_min_native CXX)
set(CMAKE_CXX_STANDARD 17)
find_package(google_cloud_cpp_storage REQUIRED)
add_executable(json_min_native json_min_native.cc)
target_link_libraries(json_min_native PRIVATE google-cloud-cpp::storage)
EOF
t native.json_min_app bash -c "cmake -S '$M/json-native' -B '$M/json-native/build' -GNinja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH='$M/gcc-json-install;$VI/x64-linux' && cmake --build '$M/json-native/build'"
B="$M/json-native/build/json_min_native"
kv size.json_min_native "$(stat -c %s "$B")"
cp "$B" /tmp/n && strip /tmp/n && kv size.json_min_native_stripped "$(stat -c %s /tmp/n)"

# gRPC path for reference (what an app using storage_grpc / other libs has).
t native.deps_grpc_extra ./vcpkg/vcpkg install --x-install-root="$VI" grpc protobuf
kv native.deps_all_installed_bytes "$(bytes "$VI")"
t native.gcc_storage_grpc gcc_build "$M/gcc-grpc" "storage;storage_grpc;-monitoring;-trace;-opentelemetry;-universe_domain"
kv size.libgoogle_cloud_cpp_storage_grpc_a "$(stat -c %s "$M/gcc-grpc-install/lib/libgoogle_cloud_cpp_storage_grpc.a" 2>/dev/null || echo 0)"
kv native.gcc_grpc_install_bytes "$(bytes "$M/gcc-grpc-install")"
kv vcpkg.buildtrees_bytes "$(bytes "$M/vcpkg/buildtrees")"
kv vcpkg.downloads_bytes "$(bytes "$VCPKG_DOWNLOADS")"

# ---------------------------------------- 5. incremental rebuild (dev loop)
touch "$M/poc/rust/src/lib.rs"
t rust.incremental_rebuild cmake --build "$M/poc/build" --target json_min_wrapped -j "$J"
touch "$M/google-cloud-cpp/google/cloud/storage/client.cc"
t native.incremental_rebuild cmake --build "$M/gcc-json" -j "$J"

kv done "$(date -u +%FT%TZ)"
