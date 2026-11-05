# Third-party versions

## Vendored (pinned in-tree)

| Dependency | Version | Source | License |
| --- | --- | --- | --- |
| wgpu-native | v29.0.1.1 (`6aed509`), plus the [patches below](#patches) | git submodule `third_party/wgpu-native` | MIT / Apache-2.0 |
| webgpu-headers | `673658b` (wgpu-native's submodule) | `third_party/wgpu-native/ffi/webgpu-headers` | BSD-3-Clause |
| Rust crates for wgpu-native | pinned by its `Cargo.lock` | `tools/vendor-rust.sh` -> `third_party/rust-vendor/` (gitignored) | permissive (see `cargo deny` config upstream) |
| oxc 0.157.0 and its crates, for axe-tsx (build time only) | pinned by `tools/axe-tsx/Cargo.lock` (90 crates) | the same script and directory | MIT, Apache-2.0, BSD-3-Clause, Unicode-3.0 (`self_cell` is Apache-2.0 OR GPL-2.0-only: taken as Apache-2.0) |
| Corrosion | v0.6.1 (`1499b14`) | copied into `third_party/corrosion` | MIT |
| HarfBuzz | 15.0.0 (`35f656a`) | git submodule `third_party/harfbuzz`, built by `third_party/harfbuzz.cmake` | Old MIT |
| QuickJS-ng | v0.18.0 (`d7215b0`) | git submodule `third_party/quickjs-ng`, built by `third_party/quickjs.cmake` (engine only: no `quickjs-libc`, no `qjs`/`qjsc`) | MIT |
| Luau | 0.745 (`2a3d893`) | git submodule `third_party/luau`, built by `third_party/luau.cmake` (VM and compiler only: no analyzer, no native code generation, no tools); `bindings/luau` only | MIT |

wgpu-native is built with `--no-default-features --features wgsl,vulkan` (Linux needs only
Vulkan and WGSL) and always in the `release` profile.

## Patches

The submodule stays the pinned upstream commit. At configure time, `third_party/CMakeLists.txt`
copies it to `<build>/third_party/wgpu-native-patched`, applies `third_party/patches/wgpu-native-*.patch`
in name order, and builds that copy. A patch that no longer applies stops the configure.

Every patch has an upstream pull request. Delete the patch when bumping to a release that
includes it.

| Patch | What it does | Upstream | Status |
| --- | --- | --- | --- |
| `wgpu-native-device-lost.patch` | `wgpuQueueSubmit` and `wgpuSurfaceGetCurrentTexture` report errors through the error sink instead of aborting. `tools/vk-lost-layer` tests it in CI | [gfx-rs/wgpu-native#641](https://github.com/gfx-rs/wgpu-native/pull/641) | Open |

## Data

| Data | Version | Source | License |
| --- | --- | --- | --- |
| Test fonts (Noto Sans, Hebrew, Arabic, Devanagari; Noto Sans SC subset) | google/fonts `6c1dbe2` | `tests/fonts/` (see `SOURCES.md`) | OFL 1.1 |
| Unicode Character Database | 18.0.0 | `tools/unicode/fetch.sh` (sha256-pinned) -> generated `src/unicode/ucd_tables.{h,c}`; conformance files in `tests/unicode/` | Unicode License v3 (`tests/unicode/LICENSE.txt`) |

## Toolchains

| Tool | Version |
| --- | --- |
| Rust | 1.97 (`rust-toolchain.toml`: one toolchain for wgpu-native and axe-tsx; wgpu-native pins 1.93 but builds on it) |
| Clang / Clang++ | 19 or newer (C23, `#embed`); CI pins 22, developed with 22–23. C++ only for HarfBuzz and Luau (C++17) |
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
