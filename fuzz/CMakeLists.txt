# libFuzzer targets (fuzz preset). Each runs as a short CTest smoke test from its seed
# corpus; new inputs go to the build directory, never into the committed corpus. Run one
# for longer by hand: build/fuzz/fuzz/fuzz_bidi -max_total_time=600 fuzz/corpus/bidi
foreach(name IN ITEMS utf8 linebreak bidi set_text)
    add_executable(fuzz_${name} fuzz_${name}.c)
    target_include_directories(fuzz_${name} PRIVATE ${PROJECT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(fuzz_${name} PRIVATE UI_FUZZ_FONTS="${PROJECT_SOURCE_DIR}/tests/fonts")
    target_link_libraries(fuzz_${name} PRIVATE ui m)
    target_link_options(fuzz_${name} PRIVATE -fsanitize=fuzzer)
    ui_target_warnings(fuzz_${name})
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/corpus/${name})
    add_test(NAME fuzz_${name}
             COMMAND fuzz_${name} -runs=10000 -seed=1 -max_len=8192
                     ${CMAKE_CURRENT_BINARY_DIR}/corpus/${name}
                     ${CMAKE_CURRENT_SOURCE_DIR}/corpus/${name})
    set_tests_properties(fuzz_${name} PROPERTIES TIMEOUT 600)
endforeach()
