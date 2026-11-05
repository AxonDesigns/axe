# Luau, the second language binding (bindings/luau, docs/architecture.md "Language bindings"),
# built by this project-owned file: the VM and the compiler, and what they need. Not built: the
# analyzer, native code generation, Require and the command-line tools. Only bindings/luau/
# includes lua.h, and `ui` doesn't link it.
set(_luau_dir "${CMAKE_CURRENT_SOURCE_DIR}/luau")
if(NOT EXISTS "${_luau_dir}/VM/src/lvmexecute.cpp")
    message(FATAL_ERROR "third_party/luau is missing: run `git submodule update --init`")
endif()

file(GLOB _luau_sources "${_luau_dir}/Common/src/*.cpp" "${_luau_dir}/Ast/src/*.cpp"
     "${_luau_dir}/Bytecode/src/*.cpp" "${_luau_dir}/Compiler/src/*.cpp" "${_luau_dir}/VM/src/*.cpp")
add_library(luau STATIC ${_luau_sources})
target_include_directories(luau SYSTEM PUBLIC "${_luau_dir}/VM/include" "${_luau_dir}/Compiler/include")
target_include_directories(luau PRIVATE "${_luau_dir}/Common/include" "${_luau_dir}/Ast/include"
                           "${_luau_dir}/Bytecode/include" "${_luau_dir}/Compiler/src" "${_luau_dir}/VM/src")
# Always optimized, as QuickJS-ng is: an unoptimized interpreter makes debug runs of apps and tests
# several times slower. Debug info is kept; sanitizer flags still apply (UI_SANITIZE).
target_compile_options(luau PRIVATE -O2 -g -w -fno-math-errno)
set_target_properties(luau PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
# The binding is C, so its API has C linkage (the headers leave that to the build: these apply to
# Luau's own C++ only, where `extern "C"` means something), and errors are longjmp, not exceptions,
# which couldn't cross C frames. Upstream's LUAU_EXTERN_C does the same.
target_compile_definitions(luau PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:LUA_API=extern \"C\">"
                           "$<$<COMPILE_LANGUAGE:CXX>:LUACODE_API=extern \"C\">" LUA_USE_LONGJMP=1)
target_link_libraries(luau PUBLIC m)
