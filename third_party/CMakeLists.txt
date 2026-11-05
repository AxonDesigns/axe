# Pinned third-party code. Versions and system minimums: VERSIONS.md.

# Corrosion drives Cargo for wgpu-native. The Rust crate always builds in release:
# a debug wgpu is too slow to pace frames and is not what we debug here.
# One toolchain for every crate: rust-toolchain.toml's channel. Forced, so a build directory
# configured under an older pin follows the file.
file(STRINGS "${PROJECT_SOURCE_DIR}/rust-toolchain.toml" _channel REGEX "^channel *= *\"[^\"]+\"")
string(REGEX REPLACE "^channel *= *\"([^\"]+)\".*" "\\1" _channel "${_channel}")
if(NOT _channel)
    message(FATAL_ERROR "rust-toolchain.toml has no channel")
endif()
set(Rust_TOOLCHAIN "${_channel}" CACHE STRING "rustup toolchain (from rust-toolchain.toml)" FORCE)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/corrosion/cmake")
include(Corrosion)

set(_wgpu_dir "${CMAKE_CURRENT_SOURCE_DIR}/wgpu-native")
if(NOT EXISTS "${_wgpu_dir}/Cargo.toml")
    message(FATAL_ERROR "third_party/wgpu-native is missing: run `git submodule update --init` "
                        "and tools/vendor-rust.sh")
endif()
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/rust-vendor")
    message(FATAL_ERROR "Vendored Rust crates are missing: run tools/vendor-rust.sh")
endif()

# wgpu-native builds from a copy in the build directory with patches/*.patch applied, so the
# submodule stays the pinned upstream commit. The copy is only redone when an input changes:
# each redo touches the patched files, which would rebuild the crate. Each patch is listed in
# VERSIONS.md with its upstream pull request, and goes once a release includes it.
find_package(Git REQUIRED)
set(_wgpu_copy "${CMAKE_CURRENT_BINARY_DIR}/wgpu-native-patched")
file(GLOB _wgpu_patches CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/patches/wgpu-native-*.patch")
list(SORT _wgpu_patches)
file(GLOB_RECURSE _wgpu_inputs CONFIGURE_DEPENDS LIST_DIRECTORIES false
    "${_wgpu_dir}/src/*" "${_wgpu_dir}/ffi/*.h" "${_wgpu_dir}/ffi/*.yml")
list(SORT _wgpu_inputs)
list(APPEND _wgpu_inputs "${_wgpu_dir}/build.rs" "${_wgpu_dir}/Cargo.toml" "${_wgpu_dir}/Cargo.lock")
set(_wgpu_stamp "")
foreach(_input IN LISTS _wgpu_inputs _wgpu_patches)
    file(SHA256 "${_input}" _hash)
    string(APPEND _wgpu_stamp "${_hash}\n")
endforeach()
string(SHA256 _wgpu_stamp "${_wgpu_stamp}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_wgpu_inputs} ${_wgpu_patches})
set(_wgpu_stamp_file "${_wgpu_copy}.stamp")
set(_wgpu_old_stamp "")
if(EXISTS "${_wgpu_stamp_file}")
    file(READ "${_wgpu_stamp_file}" _wgpu_old_stamp)
endif()
if(NOT _wgpu_stamp STREQUAL _wgpu_old_stamp)
    # The stamp goes first: after a patch fails, no stamp may vouch for a half-patched copy.
    file(REMOVE "${_wgpu_stamp_file}")
    file(REMOVE_RECURSE "${_wgpu_copy}")
    file(COPY "${_wgpu_dir}/" DESTINATION "${_wgpu_copy}" PATTERN ".git" EXCLUDE)
    foreach(_patch IN LISTS _wgpu_patches)
        # The ceiling keeps git from finding this repository around the copy: inside one,
        # `git apply` can skip files without saying so.
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E env "GIT_CEILING_DIRECTORIES=${CMAKE_CURRENT_BINARY_DIR}"
                    "${GIT_EXECUTABLE}" apply --whitespace=nowarn "${_patch}"
            WORKING_DIRECTORY "${_wgpu_copy}"
            RESULT_VARIABLE _result ERROR_VARIABLE _error)
        if(_result)
            message(FATAL_ERROR "${_patch} doesn't apply to third_party/wgpu-native: "
                                "update it, or drop it if a release has it (VERSIONS.md).\n${_error}")
        endif()
    endforeach()
    file(WRITE "${_wgpu_stamp_file}" "${_wgpu_stamp}")
endif()

corrosion_import_crate(
    MANIFEST_PATH "${_wgpu_copy}/Cargo.toml"
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
if(UI_BUILD_LUAU)
    include(${CMAKE_CURRENT_SOURCE_DIR}/luau.cmake)
endif()
