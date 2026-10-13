#!/usr/bin/env bash
# One-time setup of the GCS C++/Rust POC benchmark on a GCE VM (Debian 12).
# Everything lives under ~/gcs-poc; nothing outside it is modified except
# ~/.local/rust (Rust toolchain) and ~/.cargo (crate cache).
set -euo pipefail

ROOT="$HOME/gcs-poc"
RUST_VERSION=1.99.0
RUST_PREFIX="$HOME/.local/rust"
RUST_REF="${POC_RUST_REF:-poc/cpp-rust-wrapper}"   # branch containing examples/cpp-wrapper-poc
mkdir -p "$ROOT"
cd "$ROOT"

echo "== 1. Rust $RUST_VERSION (standalone installer, sha256 + GPG verified)"
if [[ ! -x "$RUST_PREFIX/bin/cargo" ]]; then
  T="rust-$RUST_VERSION-x86_64-unknown-linux-gnu"
  curl -sSfLO "https://static.rust-lang.org/dist/$T.tar.xz"
  curl -sSfLO "https://static.rust-lang.org/dist/$T.tar.xz.sha256"
  curl -sSfLO "https://static.rust-lang.org/dist/$T.tar.xz.asc"
  curl -sSfL  "https://static.rust-lang.org/rust-key.gpg.ascii" -o rust-key.gpg.ascii
  sha256sum -c "$T.tar.xz.sha256"
  export GNUPGHOME="$ROOT/.gnupg-rust"; mkdir -p "$GNUPGHOME"; chmod 700 "$GNUPGHOME"
  gpg --batch --import rust-key.gpg.ascii
  gpg --batch --verify "$T.tar.xz.asc" "$T.tar.xz"
  tar xf "$T.tar.xz"
  "./$T/install.sh" --prefix="$RUST_PREFIX" --disable-ldconfig --components=rustc,cargo,rust-std-x86_64-unknown-linux-gnu
  rm -rf "$T" "$T.tar.xz"
fi
export PATH="$RUST_PREFIX/bin:$HOME/.cargo/bin:$PATH"
cargo --version; rustc --version

echo "== 2. cbindgen"
command -v cbindgen >/dev/null || cargo install cbindgen --locked --version 0.29.2
cbindgen --version

echo "== 3. google-cloud-rust (fork) @ $RUST_REF"
if [[ ! -d google-cloud-rust ]]; then
  git clone -q https://github.com/v-pratap/google-cloud-rust.git
fi
git -C google-cloud-rust fetch -q origin "$RUST_REF" && git -C google-cloud-rust checkout -q FETCH_HEAD
git -C google-cloud-rust log --oneline -1

echo "== 4. google-cloud-cpp (clean upstream, latest release tag)"
if [[ ! -d google-cloud-cpp ]]; then
  TAG=$(git ls-remote --tags --refs --sort=-v:refname https://github.com/googleapis/google-cloud-cpp.git 'v*' |
        awk -F/ '$3 ~ /^v[0-9]+\.[0-9]+\.[0-9]+$/ {print $3}' | head -1)
  echo "tag: $TAG"
  git clone -q --depth 1 --branch "$TAG" https://github.com/googleapis/google-cloud-cpp.git
fi
git -C google-cloud-cpp log --oneline -1

echo "== 5. POC sources (examples/cpp-wrapper-poc inside the fork)"
ln -sfn "$ROOT/google-cloud-rust/examples/cpp-wrapper-poc" "$ROOT/gcs-cpp-rust-poc"
echo "setup done"
