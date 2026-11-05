# Developer tools; none is part of the default build.
add_executable(font_subset EXCLUDE_FROM_ALL font_subset.c ${PROJECT_SOURCE_DIR}/src/unicode/utf8.c)
target_include_directories(font_subset PRIVATE ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(font_subset PRIVATE harfbuzz_subset)
ui_target_warnings(font_subset)

add_executable(text_contrast_shots EXCLUDE_FROM_ALL text_contrast_shots.c)
target_include_directories(text_contrast_shots PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(text_contrast_shots PRIVATE _DEFAULT_SOURCE
    AXE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_link_libraries(text_contrast_shots PRIVATE ui m)
ui_target_warnings(text_contrast_shots)

# The script toolchain (docs/typescript.md): axe-jsc compiles bundles to bytecode at build
# time; axe-run runs a bundle.
add_executable(axe-jsc axe-jsc/main.c)
target_include_directories(axe-jsc PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(axe-jsc PRIVATE ui)
ui_target_warnings(axe-jsc)

# Hot reload's tools: the rebuild and the watcher, shared with tests.
if(AXE_DEV)
    add_library(axe_dev STATIC axe-run/dev/build.c axe-run/dev/session.c axe-run/dev/watch_inotify.c)
    target_include_directories(axe_dev PUBLIC axe-run/dev ${PROJECT_SOURCE_DIR}/src)
    target_compile_definitions(axe_dev PRIVATE _GNU_SOURCE
        PUBLIC AXE_TSX_PATH="${CMAKE_BINARY_DIR}/tools/axe-tsx" AXE_ESBUILD_PATH="${AXE_ESBUILD}"
               AXE_DEV_PACKAGES="${AXE_DEV_PACKAGES}" AXE_DEV_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    target_link_libraries(axe_dev PUBLIC ui)
    add_dependencies(axe_dev axe_dev_packages)
    ui_target_warnings(axe_dev)
endif()

add_executable(axe-run axe-run/main.c)
target_compile_definitions(axe-run PRIVATE _DEFAULT_SOURCE)
target_link_libraries(axe-run PRIVATE ui $<$<BOOL:${AXE_DEV}>:axe_dev>)
ui_target_warnings(axe-run)

# axe-tsx compiles TSX for the bundler (docs/ui.md, "How TSX compiles"): a Rust tool on oxc,
# built through Corrosion from the vendored crates, like wgpu-native, and always in release.
corrosion_import_crate(MANIFEST_PATH ${PROJECT_SOURCE_DIR}/tools/axe-tsx/Cargo.toml PROFILE release LOCKED)
