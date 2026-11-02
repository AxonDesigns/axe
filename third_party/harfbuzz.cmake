# HarfBuzz, built from its amalgamated sources by this project-owned file (the architecture's
# rule for C++ dependencies; upstream's CMake build is only community-maintained). Flags
# follow upstream's meson.build. No FreeType, glib, ICU, graphite or platform shapers:
# HarfBuzz's own OpenType functions load fonts, shape, and draw outlines.
set(_hb_dir "${CMAKE_CURRENT_SOURCE_DIR}/harfbuzz")
if(NOT EXISTS "${_hb_dir}/src/harfbuzz.cc")
    message(FATAL_ERROR "third_party/harfbuzz is missing: run `git submodule update --init`")
endif()

# Always optimized, even in debug presets (as wgpu-native is always built in release): an
# unoptimized HarfBuzz shapes ~15x slower, which made a debug build's first text frame cost
# tens of milliseconds. Debug info is kept; sanitizer flags still apply.
set(_hb_flags -fno-exceptions -fno-rtti -fno-threadsafe-statics -fvisibility-inlines-hidden
    -O2 -g)
# pthread mutexes, so HarfBuzz needs no std::mutex from the C++ runtime.
set(_hb_defs HAVE_PTHREAD HAVE_ATEXIT HAVE_UNISTD_H HAVE_SYS_MMAN_H HAVE_MMAP)

add_library(harfbuzz STATIC "${_hb_dir}/src/harfbuzz.cc")
target_include_directories(harfbuzz SYSTEM PUBLIC "${_hb_dir}/src")
target_compile_options(harfbuzz PRIVATE ${_hb_flags} -w)
target_compile_definitions(harfbuzz PRIVATE ${_hb_defs})
target_compile_features(harfbuzz PRIVATE cxx_std_17)
target_link_libraries(harfbuzz PUBLIC pthread m)

# The subsetter is only used by tools/font_subset (building the CJK test font).
add_library(harfbuzz_subset STATIC EXCLUDE_FROM_ALL "${_hb_dir}/src/harfbuzz-subset.cc")
target_compile_options(harfbuzz_subset PRIVATE ${_hb_flags} -w)
target_compile_definitions(harfbuzz_subset PRIVATE ${_hb_defs})
target_compile_features(harfbuzz_subset PRIVATE cxx_std_17)
target_link_libraries(harfbuzz_subset PUBLIC harfbuzz)
