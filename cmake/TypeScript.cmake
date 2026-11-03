# TypeScript apps and tests (docs/typescript.md). esbuild bundles, axe-jsc compiles to bytecode,
# tsc type-checks. The tools come from ts/node_modules: `npm ci --ignore-scripts --prefix ts`.
# Their native binaries are used directly, so building needs no Node.js; nothing at run time
# does either.

set(AXE_TS_DIR "${PROJECT_SOURCE_DIR}/ts")
file(GLOB AXE_ESBUILD "${AXE_TS_DIR}/node_modules/@esbuild/*/bin/esbuild")
file(GLOB AXE_TSC "${AXE_TS_DIR}/node_modules/@typescript/typescript-*/lib/tsc")
if(NOT AXE_ESBUILD OR NOT AXE_TSC)
    message(FATAL_ERROR "The TypeScript tools are missing: run `npm ci --ignore-scripts --prefix ts` "
                        "(Node.js and npm are needed for that step only; see CONTRIBUTING.md)")
endif()
list(GET AXE_ESBUILD 0 AXE_ESBUILD)
list(GET AXE_TSC 0 AXE_TSC)

# What every bundle depends on: the packages' sources and the npm lockfile.
file(GLOB_RECURSE AXE_TS_PACKAGE_SOURCES CONFIGURE_DEPENDS "${AXE_TS_DIR}/packages/*/src/*.ts")
set(AXE_TS_DEPENDS ${AXE_TS_PACKAGE_SOURCES} "${AXE_TS_DIR}/package-lock.json")

# axe_ts_bundle(<output.js> <entry.ts> [DEPENDS files...])
# Bundles `entry` and everything it imports into one ES module, except the host's modules.
function(axe_ts_bundle output entry)
    cmake_parse_arguments(ARG "" "" "DEPENDS" ${ARGN})
    get_filename_component(entry_dir "${entry}" DIRECTORY)
    file(GLOB_RECURSE local_sources CONFIGURE_DEPENDS "${entry_dir}/*.ts")
    add_custom_command(
        OUTPUT "${output}"
        COMMAND "${AXE_ESBUILD}" "${entry}" --bundle --format=esm --platform=neutral --target=es2023
                --keep-names --external:axe:host --external:axe:test --external:axe:stall --log-level=warning
                "--tsconfig=${AXE_TS_DIR}/tsconfig.json"
                "--define:AXE_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\"" "--outfile=${output}"
        DEPENDS "${entry}" ${local_sources} ${AXE_TS_DEPENDS} ${ARG_DEPENDS}
        WORKING_DIRECTORY "${AXE_TS_DIR}"
        COMMENT "Bundling ${entry}"
        VERBATIM)
endfunction()

# axe_ts_bytecode(<output.axbc> <input.js>)
function(axe_ts_bytecode output input)
    get_filename_component(name "${input}" NAME)
    add_custom_command(
        OUTPUT "${output}"
        COMMAND axe-jsc "${input}" "${output}" --name "${name}"
        DEPENDS axe-jsc "${input}"
        COMMENT "Compiling ${name} to bytecode"
        VERBATIM)
endfunction()

# axe_ts_app(<name> ENTRY <main.ts> [WIDTH w] [HEIGHT h] [SCALE n] [TITLE t] [FONT path])
# An executable running the app: axe-run with the app built in. Debug builds read the bundle's
# source from the build tree (re-bundling needs no relink); other builds embed its bytecode.
function(axe_ts_app name)
    cmake_parse_arguments(ARG "" "ENTRY;WIDTH;HEIGHT;SCALE;TITLE;FONT" "" ${ARGN})
    set(js "${CMAKE_CURRENT_BINARY_DIR}/${name}.js")
    set(axbc "${CMAKE_CURRENT_BINARY_DIR}/${name}.axbc")
    axe_ts_bundle("${js}" "${ARG_ENTRY}")
    axe_ts_bytecode("${axbc}" "${js}")
    add_custom_target(${name}_bundle DEPENDS "${js}" "${axbc}")
    add_executable(${name} "${PROJECT_SOURCE_DIR}/tools/axe-run/main.c")
    target_link_libraries(${name} PRIVATE ui)
    target_compile_definitions(${name} PRIVATE _DEFAULT_SOURCE "AXE_RUN_NAME=\"${name}.js\"")
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_definitions(${name} PRIVATE "AXE_RUN_BUNDLE=\"${js}\"")
    else()
        target_compile_definitions(${name} PRIVATE "AXE_RUN_EMBED=\"${axbc}\"")
    endif()
    foreach(opt WIDTH HEIGHT SCALE)
        if(ARG_${opt})
            target_compile_definitions(${name} PRIVATE AXE_RUN_${opt}=${ARG_${opt}})
        endif()
    endforeach()
    foreach(opt TITLE FONT)
        if(ARG_${opt})
            target_compile_definitions(${name} PRIVATE "AXE_RUN_${opt}=\"${ARG_${opt}}\"")
        endif()
    endforeach()
    add_dependencies(${name} ${name}_bundle)
    ui_target_warnings(${name})
endfunction()
