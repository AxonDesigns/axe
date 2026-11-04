#!/bin/sh
# Vendors the Rust crates of every Rust build into third_party/rust-vendor/ (gitignored):
# wgpu-native's, pinned by the submodule's Cargo.lock, and axe-tsx's, pinned by
# tools/axe-tsx/Cargo.lock. Builds then run offline against .cargo/config.toml. Re-run after
# bumping the submodule or changing axe-tsx's dependencies.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
git submodule update --init third_party/wgpu-native
git -C third_party/wgpu-native submodule update --init ffi/webgpu-headers
# The repo-level .cargo/config.toml redirects crates-io to the vendor dir, so vendor from
# outside the repo with network access enabled.
cd /
CARGO_NET_OFFLINE=false cargo +1.97 vendor --locked \
    --manifest-path "$root/third_party/wgpu-native/Cargo.toml" \
    --sync "$root/tools/axe-tsx/Cargo.toml" \
    "$root/third_party/rust-vendor" > /dev/null
echo "vendored crates into third_party/rust-vendor"
