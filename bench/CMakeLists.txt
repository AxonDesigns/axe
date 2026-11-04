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

# The script host (docs/script.md): encoding from JS, startup, memory, event latency. The
# startup cases run bench/apps/cards, a frozen app (CONTRIBUTING.md, "Bench apps"), never an
# example: examples change freely, and baselines must not move with them.
axe_ts_bundle(${CMAKE_CURRENT_BINARY_DIR}/script/bench.js ${CMAKE_CURRENT_SOURCE_DIR}/script/bench.ts)
axe_ts_bundle(${CMAKE_CURRENT_BINARY_DIR}/script/empty.js ${CMAKE_CURRENT_SOURCE_DIR}/script/empty.ts)
axe_ts_bundle(${CMAKE_CURRENT_BINARY_DIR}/apps/cards.js ${CMAKE_CURRENT_SOURCE_DIR}/apps/cards/main.ts)
axe_ts_bytecode(${CMAKE_CURRENT_BINARY_DIR}/apps/cards.axbc ${CMAKE_CURRENT_BINARY_DIR}/apps/cards.js)
add_custom_target(bench_script_bundles DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/script/bench.js
                  ${CMAKE_CURRENT_BINARY_DIR}/script/empty.js ${CMAKE_CURRENT_BINARY_DIR}/apps/cards.js
                  ${CMAKE_CURRENT_BINARY_DIR}/apps/cards.axbc)
add_executable(bench_script bench_script.c)
target_include_directories(bench_script PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(bench_script PRIVATE _DEFAULT_SOURCE AXE_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    UI_BENCH_BUNDLE="${CMAKE_CURRENT_BINARY_DIR}/script/bench.js"
    UI_EMPTY_BUNDLE="${CMAKE_CURRENT_BINARY_DIR}/script/empty.js"
    UI_CARDS_JS="${CMAKE_CURRENT_BINARY_DIR}/apps/cards.js"
    UI_CARDS_AXBC="${CMAKE_CURRENT_BINARY_DIR}/apps/cards.axbc")
target_link_libraries(bench_script PRIVATE ui m)
add_dependencies(bench_script bench_script_bundles)
ui_target_warnings(bench_script)

# The language benchmarks (docs/ui.md, "Benchmarks"): frozen TSX apps under bench/apps, compiled
# by axe-tsx, bundled and compiled to bytecode.
set(_ui_apps)
foreach(app screen1k list10k text1k)
    axe_ts_bundle(${CMAKE_CURRENT_BINARY_DIR}/apps/${app}.js ${CMAKE_CURRENT_SOURCE_DIR}/apps/${app}/main.tsx)
    axe_ts_bytecode(${CMAKE_CURRENT_BINARY_DIR}/apps/${app}.axbc ${CMAKE_CURRENT_BINARY_DIR}/apps/${app}.js)
    list(APPEND _ui_apps ${CMAKE_CURRENT_BINARY_DIR}/apps/${app}.axbc)
endforeach()
axe_ts_bytecode(${CMAKE_CURRENT_BINARY_DIR}/script/empty.axbc ${CMAKE_CURRENT_BINARY_DIR}/script/empty.js)
add_custom_target(bench_ui_bundles DEPENDS ${_ui_apps} ${CMAKE_CURRENT_BINARY_DIR}/script/empty.axbc)
add_executable(bench_ui bench_ui.c)
target_include_directories(bench_ui PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(bench_ui PRIVATE _DEFAULT_SOURCE AXE_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    UI_EMPTY_AXBC="${CMAKE_CURRENT_BINARY_DIR}/script/empty.axbc"
    UI_SCREEN1K_AXBC="${CMAKE_CURRENT_BINARY_DIR}/apps/screen1k.axbc"
    UI_LIST10K_AXBC="${CMAKE_CURRENT_BINARY_DIR}/apps/list10k.axbc"
    UI_TEXT1K_AXBC="${CMAKE_CURRENT_BINARY_DIR}/apps/text1k.axbc")
target_link_libraries(bench_ui PRIVATE ui m)
add_dependencies(bench_ui bench_ui_bundles)
ui_target_warnings(bench_ui)
