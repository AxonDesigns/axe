# Pinned third-party code. Versions and system minimums: VERSIONS.md.

# Corrosion drives Cargo for wgpu-native. The Rust crate always builds in release:
# a debug wgpu is too slow to pace frames and is not what we debug here.
set(Rust_TOOLCHAIN "1.93" CACHE STRING "rustup toolchain (matches rust-toolchain.toml)")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/corrosion/cmake")
include(Corrosion)

set(_wgpu_dir "${CMAKE_CURRENT_SOURCE_DIR}/wgpu-native")
if(NOT EXISTS "${_wgpu_dir}/Cargo.toml")
    message(FATAL_ERROR "third_party/wgpu-native is missing: run `git submodule update --init` "
                        "and tools/vendor-rust.sh")
endif()
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/wgpu-native-vendor")
    message(FATAL_ERROR "Vendored Rust crates are missing: run tools/vendor-rust.sh")
endif()

corrosion_import_crate(
    MANIFEST_PATH "${_wgpu_dir}/Cargo.toml"
    PROFILE release
    CRATE_TYPES staticlib
    NO_DEFAULT_FEATURES
    FEATURES wgsl vulkan
    LOCKED)
target_link_libraries(wgpu_native INTERFACE dl m pthread)

# The only way project code reaches webgpu.h; linked privately by src/render alone.
add_library(ui_webgpu INTERFACE)
target_include_directories(ui_webgpu SYSTEM INTERFACE
    "${_wgpu_dir}/ffi" "${_wgpu_dir}/ffi/webgpu-headers")
target_link_libraries(ui_webgpu INTERFACE wgpu_native)
set(UI_WEBGPU_HEADERS "${_wgpu_dir}/ffi/webgpu-headers/webgpu.h" "${_wgpu_dir}/ffi/wgpu.h"
    PARENT_SCOPE)
include(${CMAKE_CURRENT_SOURCE_DIR}/harfbuzz.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/quickjs.cmake)
