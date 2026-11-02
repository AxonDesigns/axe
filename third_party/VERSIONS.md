# Third-party versions

## Vendored (pinned in-tree)

| Dependency | Version | Source | License |
| --- | --- | --- | --- |
| wgpu-native | v29.0.1.1 (`6aed509`) | git submodule `third_party/wgpu-native` | MIT / Apache-2.0 |
| webgpu-headers | `673658b` (wgpu-native's submodule) | `third_party/wgpu-native/ffi/webgpu-headers` | BSD-3-Clause |
| Rust crates for wgpu-native | pinned by its `Cargo.lock` | `tools/vendor-rust.sh` -> `third_party/wgpu-native-vendor/` (gitignored) | permissive (see `cargo deny` config upstream) |
| Corrosion | v0.6.1 (`1499b14`) | copied into `third_party/corrosion` | MIT |

wgpu-native is built with `--no-default-features --features wgsl,vulkan` (Linux needs only
Vulkan and WGSL) and always in the `release` profile.

## Toolchains

| Tool | Version |
| --- | --- |
| Rust | 1.93 (`rust-toolchain.toml`, matches wgpu-native's pin) |
| Clang | 19 or newer (C23, `#embed`); CI pins 22, developed with 22–23 |
| CMake | 3.28 or newer |

## System libraries (Linux, for now)

Minimums are enforced by `pkg_check_modules` in `src/CMakeLists.txt`; only the tested
versions have actually been exercised.

| Library | Minimum | Tested | Why the minimum |
| --- | --- | --- | --- |
| libwayland-client | 1.20 | 1.26.0 | thread-safe proxy wrappers and `wl_display_prepare_read` |
| wayland-protocols | 1.31 | 1.50 | first release with fractional-scale-v1 |
| wayland-scanner | 1.20 | 1.26.0 | `private-code` generation |
| xkbcommon | 1.0 | 1.13.2 | keymap from the compositor's fd |
| Vulkan loader + driver | 1.3 | 1.4.357 | wgpu's Vulkan backend; lavapipe (`vulkan-swrast`) for headless CI |
