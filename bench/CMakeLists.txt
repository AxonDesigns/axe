# Benchmarks: built in every preset, meaningful in release. Not part of CTest.
add_executable(bench_text bench_text.c)
target_include_directories(bench_text PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(bench_text PRIVATE _DEFAULT_SOURCE AXE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_link_libraries(bench_text PRIVATE ui m)
ui_target_warnings(bench_text)
