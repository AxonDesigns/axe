# QuickJS-ng, the JS engine (docs/script.md), built by this project-owned file from its four
# engine sources. Not built: quickjs-libc.c (the std and os modules, which give scripts file
# system and process access) and the qjs/qjsc programs. Only src/script/quickjs/ includes
# quickjs.h. Flags follow upstream's CMakeLists.txt.
set(_qjs_dir "${CMAKE_CURRENT_SOURCE_DIR}/quickjs-ng")
if(NOT EXISTS "${_qjs_dir}/quickjs.c")
    message(FATAL_ERROR "third_party/quickjs-ng is missing: run `git submodule update --init`")
endif()

add_library(quickjs STATIC
    "${_qjs_dir}/dtoa.c"
    "${_qjs_dir}/libregexp.c"
    "${_qjs_dir}/libunicode.c"
    "${_qjs_dir}/quickjs.c")
target_include_directories(quickjs SYSTEM PUBLIC "${_qjs_dir}")
# Always optimized, as HarfBuzz is: an unoptimized interpreter makes debug runs of apps and
# tests several times slower. Debug info is kept; sanitizer flags still apply (UI_SANITIZE).
# -funsigned-char as upstream: the engine assumes it.
target_compile_options(quickjs PRIVATE -O2 -g -funsigned-char -w)
target_compile_definitions(quickjs PRIVATE _GNU_SOURCE)
set_target_properties(quickjs PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
target_link_libraries(quickjs PUBLIC m pthread)
