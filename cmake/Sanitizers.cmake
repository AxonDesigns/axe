# Applies UI_SANITIZE to every C target. Rust code (wgpu-native) is not instrumented.
if(UI_SANITIZE)
    list(JOIN UI_SANITIZE "," _ui_sanitizers)
    add_compile_options(-fsanitize=${_ui_sanitizers} -fno-omit-frame-pointer
                        -fno-sanitize-recover=all)
    add_link_options(-fsanitize=${_ui_sanitizers})
endif()

# Fuzzing: instrument everything (HarfBuzz included) for libFuzzer's coverage feedback;
# the fuzz targets themselves link the fuzzer runtime.
if(UI_FUZZ)
    add_compile_options(-fsanitize=fuzzer-no-link)
endif()
