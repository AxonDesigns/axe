# Third-party versions

## Vendored (pinned in-tree)

| Dependency | Version | Source | License |
| --- | --- | --- | --- |
| wgpu-native | v29.0.1.1 (`6aed509`) | git submodule `third_party/wgpu-native` | MIT / Apache-2.0 |
| webgpu-headers | `673658b` (wgpu-native's submodule) | `third_party/wgpu-native/ffi/webgpu-headers` | BSD-3-Clause |
| Rust crates for wgpu-native | pinned by its `Cargo.lock` | `tools/vendor-rust.sh` -> `third_party/wgpu-native-vendor/` (gitignored) | permissive (see `cargo deny` config upstream) |
| Corrosion | v0.6.1 (`1499b14`) | copied into `third_party/corrosion` | MIT |
| HarfBuzz | 15.0.0 (`35f656a`) | git submodule `third_party/harfbuzz`, built by `third_party/harfbuzz.cmake` | Old MIT |
| QuickJS-ng | v0.18.0 (`d7215b0`) | git submodule `third_party/quickjs-ng`, built by `third_party/quickjs.cmake` (engine only: no `quickjs-libc`, no `qjs`/`qjsc`) | MIT |

wgpu-native is built with `--no-default-features --features wgsl,vulkan` (Linux needs only
Vulkan and WGSL) and always in the `release` profile.

## Data

| Data | Version | Source | License |
| --- | --- | --- | --- |
| Test fonts (Noto Sans, Hebrew, Arabic, Devanagari; Noto Sans SC subset) | google/fonts `6c1dbe2` | `tests/fonts/` (see `SOURCES.md`) | OFL 1.1 |
| Unicode Character Database | 18.0.0 | `tools/unicode/fetch.sh` (sha256-pinned) -> generated `src/unicode/ucd_tables.{h,c}`; conformance files in `tests/unicode/` | Unicode License v3 (`tests/unicode/LICENSE.txt`) |

## Toolchains

| Tool | Version |
| --- | --- |
| Rust | 1.97 (`rust-toolchain.toml`: one toolchain for wgpu-native and axe-tsx; wgpu-native pins 1.93 but builds on it) |
| Clang / Clang++ | 19 or newer (C23, `#embed`); CI pins 22, developed with 22–23. C++ only for HarfBuzz (C++17) |
| CMake | 3.28 or newer |
| Node.js | 24 (CI); npm installs the TypeScript tools only, nothing at run time uses Node |
| esbuild | 0.28.2, pinned by `ts/package-lock.json` (MIT); bundles TypeScript apps and tests |
| TypeScript | 7.0.2, pinned by `ts/package-lock.json` (Apache-2.0); type-checking only |
| Python | 3.10 or newer, stdlib only: the wire IDL generator (`tools/idl`, also run by CTest) and the Unicode tables |

## System libraries (Linux, for now)

Minimums are enforced by `pkg_check_modules` in `src/CMakeLists.txt`; only the tested
versions have actually been exercised.

| Library | Minimum | Tested | Why the minimum |
| --- | --- | --- | --- |
| libwayland-client | 1.20 | 1.26.0 | thread-safe proxy wrappers and `wl_display_prepare_read` |
| wayland-protocols | 1.31 | 1.50 | first release with fractional-scale-v1 |
| wayland-scanner | 1.20 | 1.26.0 | `private-code` generation |
| xkbcommon | 1.0 | 1.13.2 | keymap from the compositor's fd |
| Fontconfig | 2.13 | 2.18.3 | `FcWeightFromOpenTypeDouble`, `FcConfigParseAndLoadFromMemory` |
| Vulkan loader + driver | 1.3 | 1.4.357 | wgpu's Vulkan backend; lavapipe (`vulkan-swrast`) for headless CI |
