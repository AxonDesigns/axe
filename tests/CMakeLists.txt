# Unit tests reach into internal headers on purpose.
add_executable(test_runtime test_runtime.c)
target_include_directories(test_runtime PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(test_runtime PRIVATE _DEFAULT_SOURCE)
target_link_libraries(test_runtime PRIVATE ui m)
ui_target_warnings(test_runtime)
add_test(NAME runtime COMMAND test_runtime)

add_executable(test_core test_core.c)
target_include_directories(test_core PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(test_core PRIVATE _DEFAULT_SOURCE)
target_link_libraries(test_core PRIVATE ui m)
ui_target_warnings(test_core)
add_test(NAME core_pacing COMMAND test_core)

add_executable(test_headless test_headless.c)
target_include_directories(test_headless PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(test_headless PRIVATE spring_rect_logic m)
ui_target_warnings(test_headless)
# Runs on the software Vulkan driver (lavapipe) so it needs no GPU: CI-safe.
add_test(NAME headless_spring_rect COMMAND test_headless)
set_tests_properties(headless_spring_rect PROPERTIES
    ENVIRONMENT "UI_GPU=software"
    TIMEOUT 120)

# Internal-API tests that drive a full headless app.
foreach(name IN ITEMS backpressure stall)
    add_executable(test_${name} test_${name}.c)
    target_include_directories(test_${name} PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_compile_definitions(test_${name} PRIVATE _DEFAULT_SOURCE)
    target_link_libraries(test_${name} PRIVATE ui m)
    ui_target_warnings(test_${name})
    add_test(NAME ${name} COMMAND test_${name})
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "UI_GPU=software" TIMEOUT 120)
endforeach()

# The Unicode layer: unit tests, then the official conformance files (Unicode 18.0).
add_executable(test_unicode test_unicode.c)
target_include_directories(test_unicode PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(test_unicode PRIVATE ui m)
ui_target_warnings(test_unicode)
add_test(NAME unicode COMMAND test_unicode)

add_executable(test_unicode_conformance test_unicode_conformance.c)
target_include_directories(test_unicode_conformance PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(test_unicode_conformance PRIVATE ui m)
ui_target_warnings(test_unicode_conformance)
foreach(kind_file IN ITEMS "bidi:BidiCharacterTest" "linebreak:LineBreakTest"
                           "grapheme:GraphemeBreakTest")
    string(REPLACE ":" ";" kind_file "${kind_file}")
    list(GET kind_file 0 kind)
    list(GET kind_file 1 file)
    add_test(NAME unicode_${kind}
             COMMAND test_unicode_conformance ${kind} ${CMAKE_CURRENT_SOURCE_DIR}/unicode/${file}.txt)
endforeach()

# Fonts and shaping, with the bundled test fonts only.
add_executable(test_text_shape test_text_shape.c)
target_include_directories(test_text_shape PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(test_text_shape PRIVATE
    UI_TEST_FONTS="${CMAKE_CURRENT_SOURCE_DIR}/fonts")
target_link_libraries(test_text_shape PRIVATE ui m)
ui_target_warnings(test_text_shape)
add_test(NAME text_shape COMMAND test_text_shape)

# Rasterizer and glyph atlas.
add_executable(test_text_raster test_text_raster.c)
target_include_directories(test_text_raster PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(test_text_raster PRIVATE _DEFAULT_SOURCE
    UI_TEST_FONTS="${CMAKE_CURRENT_SOURCE_DIR}/fonts")
target_link_libraries(test_text_raster PRIVATE ui m)
ui_target_warnings(test_text_raster)
add_test(NAME text_raster COMMAND test_text_raster)

# Paragraph layout.
add_executable(test_text_layout test_text_layout.c)
target_include_directories(test_text_layout PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(test_text_layout PRIVATE UI_TEST_FONTS="${CMAKE_CURRENT_SOURCE_DIR}/fonts")
target_link_libraries(test_text_layout PRIVATE ui m)
ui_target_warnings(test_text_layout)
add_test(NAME text_layout COMMAND test_text_layout)

# Golden images of text, headless on lavapipe, under both schedulers.
add_executable(test_text_golden test_text_golden.c)
target_include_directories(test_text_golden PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(test_text_golden PRIVATE _DEFAULT_SOURCE
    UI_TEST_FONTS="${CMAKE_CURRENT_SOURCE_DIR}/fonts"
    UI_TEST_GOLDENS="${CMAKE_CURRENT_SOURCE_DIR}/goldens"
    UI_TEST_OUT="${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(test_text_golden PRIVATE ui m)
ui_target_warnings(test_text_golden)
add_test(NAME text_golden COMMAND test_text_golden)
set_tests_properties(text_golden PROPERTIES ENVIRONMENT "UI_GPU=software" TIMEOUT 300)

# Glyph uploads survive display-list replacement, under both schedulers.
add_executable(test_atlas_upload test_atlas_upload.c)
target_include_directories(test_atlas_upload PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(test_atlas_upload PRIVATE _DEFAULT_SOURCE
    UI_TEST_FONTS="${CMAKE_CURRENT_SOURCE_DIR}/fonts")
target_link_libraries(test_atlas_upload PRIVATE ui m)
ui_target_warnings(test_atlas_upload)
add_test(NAME atlas_upload COMMAND test_atlas_upload)
set_tests_properties(atlas_upload PROPERTIES ENVIRONMENT "UI_GPU=software" TIMEOUT 120)

# The set_text / append_text wire ops.
add_executable(test_text_wire test_text_wire.c)
target_include_directories(test_text_wire PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(test_text_wire PRIVATE ui m)
ui_target_warnings(test_text_wire)
add_test(NAME text_wire COMMAND test_text_wire)
