# Benchmarks: built in every preset, meaningful in release. Not part of CTest.
add_executable(bench_text bench_text.c)
target_include_directories(bench_text PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(bench_text PRIVATE _DEFAULT_SOURCE AXE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_link_libraries(bench_text PRIVATE ui m)
ui_target_warnings(bench_text)

add_executable(bench_layout bench_layout.c)
target_include_directories(bench_layout PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(bench_layout PRIVATE _DEFAULT_SOURCE AXE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_link_libraries(bench_layout PRIVATE ui m)
ui_target_warnings(bench_layout)

# The script host (docs/script.md): encoding from JS, startup, memory, event latency.
axe_ts_bundle(${CMAKE_CURRENT_BINARY_DIR}/script/bench.js ${CMAKE_CURRENT_SOURCE_DIR}/script/bench.ts)
axe_ts_bundle(${CMAKE_CURRENT_BINARY_DIR}/script/empty.js ${CMAKE_CURRENT_SOURCE_DIR}/script/empty.ts)
add_custom_target(bench_script_bundles DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/script/bench.js
                  ${CMAKE_CURRENT_BINARY_DIR}/script/empty.js layout_cards_ts_bundle)
add_executable(bench_script bench_script.c)
target_include_directories(bench_script PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(bench_script PRIVATE _DEFAULT_SOURCE AXE_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    UI_BENCH_BUNDLE="${CMAKE_CURRENT_BINARY_DIR}/script/bench.js"
    UI_EMPTY_BUNDLE="${CMAKE_CURRENT_BINARY_DIR}/script/empty.js"
    UI_CARDS_JS="${CMAKE_BINARY_DIR}/examples/layout_cards_ts.js"
    UI_CARDS_AXBC="${CMAKE_BINARY_DIR}/examples/layout_cards_ts.axbc")
target_link_libraries(bench_script PRIVATE ui m)
add_dependencies(bench_script bench_script_bundles)
ui_target_warnings(bench_script)
