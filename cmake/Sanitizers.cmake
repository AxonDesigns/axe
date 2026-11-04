# Applies UI_SANITIZE to every C target. Rust code (wgpu-native) is not instrumented.
if(UI_SANITIZE)
    list(JOIN UI_SANITIZE "," _ui_sanitizers)
    add_compile_options(-fsanitize=${_ui_sanitizers} -fno-omit-frame-pointer
                        -fno-sanitize-recover=all)
    add_link_options(-fsanitize=${_ui_sanitizers})
    # ThreadSanitizer can't see glibc's C11 threads: map them onto pthreads in every C file
    # (tools/tsan/c11_shim.h).
    if("thread" IN_LIST UI_SANITIZE)
        add_compile_options("$<$<COMPILE_LANGUAGE:C>:SHELL:-include ${PROJECT_SOURCE_DIR}/tools/tsan/c11_shim.h>")
    endif()
endif()

# Fuzzing: instrument everything (HarfBuzz included) for libFuzzer's coverage feedback;
# the fuzz targets themselves link the fuzzer runtime.
if(UI_FUZZ)
    add_compile_options(-fsanitize=fuzzer-no-link)
endif()
