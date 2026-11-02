# Developer tools; none is part of the default build.
add_executable(font_subset EXCLUDE_FROM_ALL font_subset.c ${PROJECT_SOURCE_DIR}/src/unicode/utf8.c)
target_include_directories(font_subset PRIVATE ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(font_subset PRIVATE harfbuzz_subset)
ui_target_warnings(font_subset)
