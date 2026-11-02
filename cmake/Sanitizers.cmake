# Applies UI_SANITIZE to every C target. Rust code (wgpu-native) is not instrumented.
if(UI_SANITIZE)
    list(JOIN UI_SANITIZE "," _ui_sanitizers)
    add_compile_options(-fsanitize=${_ui_sanitizers} -fno-omit-frame-pointer
                        -fno-sanitize-recover=all)
    add_link_options(-fsanitize=${_ui_sanitizers})
endif()
