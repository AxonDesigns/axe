# Strict warnings for project-owned code only (never applied to third_party).
function(ui_target_warnings target)
    target_compile_options(${target} PRIVATE
        -Wall -Wextra -Wpedantic -Werror
        -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wconversion -Wno-sign-conversion
        -Wimplicit-fallthrough -Wundef -Wcast-qual -Wvla -Wformat=2
        -Wno-gnu-zero-variadic-macro-arguments)
endfunction()
