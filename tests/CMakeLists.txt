# Sanitizer builds keep shared objects loaded until exit, so leak reports name the GPU
# driver libraries that tools/lsan.supp suppresses (tests/keep_loaded.c; the object library
# is defined at the root, for the examples the tests run too).
if("address" IN_LIST UI_SANITIZE)
    link_libraries(ui_keep_loaded)
endif()

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
foreach(name IN ITEMS backpressure stall clock_event)
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

# Wire evolution: buffers from older and newer writers apply like this writer's own.
add_executable(test_wire_compat test_wire_compat.c)
target_include_directories(test_wire_compat PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(test_wire_compat PRIVATE ui m)
ui_target_warnings(test_wire_compat)
add_test(NAME wire_compat COMMAND test_wire_compat)

# The generated codec reads the fixture buffers as the pre-IDL code did (tests/wire/proof.md).
add_executable(test_wire_fixtures test_wire_fixtures.c wire/wire_dump.c)
target_include_directories(test_wire_fixtures PRIVATE ${PROJECT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/wire)
target_compile_definitions(test_wire_fixtures PRIVATE UI_WIRE_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/wire/fixtures")
target_link_libraries(test_wire_fixtures PRIVATE ui m)
ui_target_warnings(test_wire_fixtures)
add_test(NAME wire_fixtures COMMAND test_wire_fixtures)

# Hit testing.
add_executable(test_text_hittest test_text_hittest.c)
target_include_directories(test_text_hittest PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(test_text_hittest PRIVATE UI_TEST_FONTS="${CMAKE_CURRENT_SOURCE_DIR}/fonts")
target_link_libraries(test_text_hittest PRIVATE ui m)
ui_target_warnings(test_text_hittest)
add_test(NAME text_hittest COMMAND test_text_hittest)

# Clipping text and descendants to a node.
add_executable(test_clip test_clip.c)
target_include_directories(test_clip PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(test_clip PRIVATE _DEFAULT_SOURCE UI_TEST_FONTS="${CMAKE_CURRENT_SOURCE_DIR}/fonts")
target_link_libraries(test_clip PRIVATE ui m)
ui_target_warnings(test_clip)
add_test(NAME clip COMMAND test_clip)
set_tests_properties(clip PROPERTIES ENVIRONMENT "UI_GPU=software" TIMEOUT 120)

# Loading fonts at runtime.
add_executable(test_font_loading test_font_loading.c)
target_include_directories(test_font_loading PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(test_font_loading PRIVATE _DEFAULT_SOURCE UI_TEST_FONTS="${CMAKE_CURRENT_SOURCE_DIR}/fonts")
target_link_libraries(test_font_loading PRIVATE ui m)
ui_target_warnings(test_font_loading)
add_test(NAME font_loading COMMAND test_font_loading)
set_tests_properties(font_loading PROPERTIES ENVIRONMENT "UI_GPU=software" TIMEOUT 120)

# The worker pool, and command buffers prepared off the core thread.
add_executable(test_jobs test_jobs.c)
target_include_directories(test_jobs PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(test_jobs PRIVATE _DEFAULT_SOURCE)
target_link_libraries(test_jobs PRIVATE ui m)
ui_target_warnings(test_jobs)
add_test(NAME jobs COMMAND test_jobs)

add_executable(test_prepare test_prepare.c)
target_include_directories(test_prepare PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(test_prepare PRIVATE _DEFAULT_SOURCE UI_TEST_FONTS="${CMAKE_CURRENT_SOURCE_DIR}/fonts")
target_link_libraries(test_prepare PRIVATE ui m)
ui_target_warnings(test_prepare)
add_test(NAME prepare COMMAND test_prepare)
set_tests_properties(prepare PROPERTIES ENVIRONMENT "UI_GPU=software" TIMEOUT 120)

# Golden images of borders, opacity groups and clipping; snapping at rest.
add_executable(test_paint_golden test_paint_golden.c)
target_include_directories(test_paint_golden PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(test_paint_golden PRIVATE _DEFAULT_SOURCE
    UI_TEST_FONTS="${CMAKE_CURRENT_SOURCE_DIR}/fonts"
    UI_TEST_GOLDENS="${CMAKE_CURRENT_SOURCE_DIR}/goldens"
    UI_TEST_OUT="${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(test_paint_golden PRIVATE ui m)
ui_target_warnings(test_paint_golden)
add_test(NAME paint_golden COMMAND test_paint_golden)
set_tests_properties(paint_golden PROPERTIES ENVIRONMENT "UI_GPU=software" TIMEOUT 300)

# Styles: precedence, state variants, tokens, transitions.
add_executable(test_style test_style.c)
target_include_directories(test_style PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(test_style PRIVATE _DEFAULT_SOURCE)
target_link_libraries(test_style PRIVATE ui m)
ui_target_warnings(test_style)
add_test(NAME style COMMAND test_style)

add_executable(test_template test_template.c)
target_include_directories(test_template PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(test_template PRIVATE _DEFAULT_SOURCE)
target_link_libraries(test_template PRIVATE ui m)
ui_target_warnings(test_template)
add_test(NAME template COMMAND test_template)

# Token scopes: nested, state blocks, use-site aliases, cycles, kinds, moves, transitions.
add_executable(test_scopes test_scopes.c)
target_include_directories(test_scopes PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(test_scopes PRIVATE _DEFAULT_SOURCE)
target_link_libraries(test_scopes PRIVATE ui m)
ui_target_warnings(test_scopes)
add_test(NAME scopes COMMAND test_scopes)

# Layout against Chrome's results for the fixtures in tools/layout-fixtures.
add_executable(test_layout_conformance test_layout_conformance.c layout/fixtures_generated.c)
target_include_directories(test_layout_conformance PRIVATE ${PROJECT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(test_layout_conformance PRIVATE _DEFAULT_SOURCE
    UI_TEST_FONTS="${CMAKE_CURRENT_SOURCE_DIR}/fonts")
target_link_libraries(test_layout_conformance PRIVATE ui m)
ui_target_warnings(test_layout_conformance)
add_test(NAME layout_conformance COMMAND test_layout_conformance)

# Layout on the scene: root, set_rect, display none, move_node, text, incremental relayout.
add_executable(test_layout test_layout.c)
target_include_directories(test_layout PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(test_layout PRIVATE _DEFAULT_SOURCE UI_TEST_FONTS="${CMAKE_CURRENT_SOURCE_DIR}/fonts")
target_link_libraries(test_layout PRIVATE ui m)
ui_target_warnings(test_layout)
add_test(NAME layout COMMAND test_layout)

# Layout animations: the presentation rect, retargeting, reparenting, both schedulers.
add_executable(test_layout_anim test_layout_anim.c)
target_include_directories(test_layout_anim PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(test_layout_anim PRIVATE _DEFAULT_SOURCE
    UI_TEST_FONTS="${CMAKE_CURRENT_SOURCE_DIR}/fonts"
    UI_TEST_GOLDENS="${CMAKE_CURRENT_SOURCE_DIR}/goldens"
    UI_TEST_OUT="${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(test_layout_anim PRIVATE ui m)
ui_target_warnings(test_layout_anim)
add_test(NAME layout_anim COMMAND test_layout_anim)
set_tests_properties(layout_anim PROPERTIES ENVIRONMENT "UI_GPU=software" TIMEOUT 120)

# Measurement: request_layout and layout_measured.
add_executable(test_measure test_measure.c)
target_include_directories(test_measure PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(test_measure PRIVATE _DEFAULT_SOURCE)
target_link_libraries(test_measure PRIVATE ui m)
ui_target_warnings(test_measure)
add_test(NAME measure COMMAND test_measure)
set_tests_properties(measure PROPERTIES ENVIRONMENT "UI_GPU=software" TIMEOUT 120)

# The wire IDL's generator (tools/idl): its own tests, and the committed output must be what
# idl/wire.idl generates (docs/idl.md).
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
add_test(NAME idl_generator COMMAND Python3::Interpreter -I ${PROJECT_SOURCE_DIR}/tools/idl/test_axe_idl.py)
add_test(NAME idl_up_to_date COMMAND Python3::Interpreter -I ${PROJECT_SOURCE_DIR}/tools/idl/axe_idl.py --check)

# The JS engine interface on QuickJS-ng (src/script/engine.h).
add_executable(test_js_engine test_js_engine.c)
target_include_directories(test_js_engine PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(test_js_engine PRIVATE ui m)
ui_target_warnings(test_js_engine)
add_test(NAME js_engine COMMAND test_js_engine)

# The script host with raw JS bundles in a headless app (docs/script.md).
add_executable(test_script_host test_script_host.c)
target_include_directories(test_script_host PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(test_script_host PRIVATE ui m)
ui_target_warnings(test_script_host)
add_test(NAME script_host COMMAND test_script_host)
set_tests_properties(script_host PROPERTIES ENVIRONMENT "UI_GPU=software" TIMEOUT 120)

# TypeScript: each test file is bundled and run inside the script host by axe-test-js,
# headless on a paused clock (docs/typescript.md, "Tests"); tsc type-checks every package,
# test and example with strict settings.
add_executable(axe-test-js ts/runner.c)
target_include_directories(axe-test-js PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(axe-test-js PRIVATE AXE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_link_libraries(axe-test-js PRIVATE ui)
ui_target_warnings(axe-test-js)

function(axe_ts_test name file)
    set(js "${CMAKE_CURRENT_BINARY_DIR}/ts/${name}.js")
    axe_ts_bundle("${js}" "${file}")
    add_custom_target(ts_${name}_bundle ALL DEPENDS "${js}")
    add_test(NAME ts_${name} COMMAND axe-test-js "${js}")
    set_tests_properties(ts_${name} PROPERTIES ENVIRONMENT "UI_GPU=software" TIMEOUT 300)
endfunction()
file(GLOB _ts_tests CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/ts/packages/*/test/*.test.ts")
foreach(file IN LISTS _ts_tests)
    get_filename_component(name "${file}" NAME_WE)
    axe_ts_test(${name} "${file}")
endforeach()

add_test(NAME ts_typecheck COMMAND "${AXE_TSC}" -p "${PROJECT_SOURCE_DIR}/ts/tsconfig.json")

# The C and TypeScript layout_cards draw byte-identical images at the same times, under both
# schedulers (the shuffles, theme switches and direction changes included).
if(UI_BUILD_EXAMPLES)
    add_test(NAME layout_cards_equivalence
             COMMAND ${CMAKE_COMMAND} -DC=$<TARGET_FILE:layout_cards> -DTS=$<TARGET_FILE:layout_cards_ts>
                     -DOUT=${CMAKE_CURRENT_BINARY_DIR}/equivalence "-DTIMES=0;1.3;2.6;4.1;6.2;12.4"
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/ts/equivalence.cmake)
    add_test(NAME layout_cards_equivalence_dark_rtl
             COMMAND ${CMAKE_COMMAND} -DC=$<TARGET_FILE:layout_cards> -DTS=$<TARGET_FILE:layout_cards_ts>
                     -DOUT=${CMAKE_CURRENT_BINARY_DIR}/equivalence-dark-rtl "-DTIMES=0;3.3"
                     "-DARGS=--dark;--rtl" -P ${CMAKE_CURRENT_SOURCE_DIR}/ts/equivalence.cmake)
    add_test(NAME spring_rect_ts COMMAND spring_rect_ts --headless --at 5 --strict
             --out ${CMAKE_CURRENT_BINARY_DIR}/spring_rect_ts.ppm)
    set_tests_properties(layout_cards_equivalence layout_cards_equivalence_dark_rtl spring_rect_ts
                         PROPERTIES ENVIRONMENT "UI_GPU=software" TIMEOUT 600)
endif()

# A busy JS loop doesn't stop frames or springs (the C version: test_stall).
axe_ts_bundle(${CMAKE_CURRENT_BINARY_DIR}/ts/stall.js ${CMAKE_CURRENT_SOURCE_DIR}/ts/stall.ts)
add_custom_target(ts_stall_bundle ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/ts/stall.js)
add_executable(test_stall_js test_stall_js.c)
target_include_directories(test_stall_js PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(test_stall_js PRIVATE _DEFAULT_SOURCE UI_STALL_BUNDLE="${CMAKE_CURRENT_BINARY_DIR}/ts/stall.js")
target_link_libraries(test_stall_js PRIVATE ui m)
add_dependencies(test_stall_js ts_stall_bundle)
ui_target_warnings(test_stall_js)
add_test(NAME stall_js COMMAND test_stall_js)
set_tests_properties(stall_js PROPERTIES ENVIRONMENT "UI_GPU=software" TIMEOUT 120)

# The C API as callers use it (docs/c-api.md): a caller written with designated initializers
# compiles with -Werror as C11, C17 and C23, and a positional initializer of a grown struct
# doesn't (it is unsupported).
set(UI_C_API_FLAGS -Wall -Wextra -Wpedantic -Werror -Wmissing-field-initializers -fsyntax-only
    -I${PROJECT_SOURCE_DIR}/include)
foreach(std c11 c17 c23)
    add_test(NAME c_api_designated_${std}
             COMMAND ${CMAKE_C_COMPILER} -std=${std} ${UI_C_API_FLAGS} ${CMAKE_CURRENT_SOURCE_DIR}/c_api/designated.c)
endforeach()
add_test(NAME c_api_positional_rejected
         COMMAND ${CMAKE_C_COMPILER} -std=c23 ${UI_C_API_FLAGS} ${CMAKE_CURRENT_SOURCE_DIR}/c_api/positional.c)
# Fails for the right reason: the warning, made an error (clang and gcc wordings).
set_tests_properties(c_api_positional_rejected PROPERTIES
    PASS_REGULAR_EXPRESSION "error: missing .*tokens")
