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
