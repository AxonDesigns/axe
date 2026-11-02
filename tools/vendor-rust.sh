#!/bin/sh
# Vendors the Rust crates wgpu-native depends on into third_party/wgpu-native-vendor/
# (gitignored). Crate versions and checksums are pinned by the submodule's Cargo.lock;
# builds then run offline against .cargo/config.toml. Re-run after bumping the submodule.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
git submodule update --init third_party/wgpu-native
git -C third_party/wgpu-native submodule update --init ffi/webgpu-headers
# The repo-level .cargo/config.toml redirects crates-io to the vendor dir, so vendor from
# outside the repo with network access enabled.
cd /
CARGO_NET_OFFLINE=false cargo +1.93 vendor --locked \
    --manifest-path "$root/third_party/wgpu-native/Cargo.toml" \
    "$root/third_party/wgpu-native-vendor" > /dev/null
echo "vendored crates into third_party/wgpu-native-vendor"
