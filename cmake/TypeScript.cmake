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

# Every workspace package must be in the lockfile, or `npm ci` (CI's install) refuses to run.
# The build itself resolves packages through tsconfig paths, so it wouldn't notice.
file(READ "${AXE_TS_DIR}/package-lock.json" _lock)
file(GLOB _ts_packages "${AXE_TS_DIR}/packages/*/package.json")
foreach(_pkg IN LISTS _ts_packages)
    file(READ "${_pkg}" _json)
    string(JSON _name GET "${_json}" name)
    string(JSON _entry ERROR_VARIABLE _missing GET "${_lock}" packages "node_modules/${_name}")
    if(_missing)
        message(FATAL_ERROR "${_name} is missing from ts/package-lock.json: run "
                            "`npm install --ignore-scripts --prefix ts` and commit the lockfile")
    endif()
endforeach()

# What every bundle depends on: the packages' sources and the npm lockfile.
file(GLOB_RECURSE AXE_TS_PACKAGE_SOURCES CONFIGURE_DEPENDS "${AXE_TS_DIR}/packages/*/src/*.ts" "${AXE_TS_DIR}/packages/*/src/*.tsx")
set(AXE_TS_DEPENDS ${AXE_TS_PACKAGE_SOURCES} "${AXE_TS_DIR}/package-lock.json")

# axe_ts_bundle(<output.js> <entry.ts|tsx> [ROOT dir] [DEPENDS files...])
# Bundles `entry` and everything it imports into one ES module, except the host's modules. When
# the app's sources (ROOT, the entry's directory by default) include TSX, axe-tsx compiles them
# first into a mirror of ROOT in the build tree (docs/ui.md, "How TSX compiles"), which esbuild
# bundles; *.check.ts(x) files are type tests and stay out. Imports leaving ROOT must go through
# package names (tsconfig paths), not relative paths.
function(axe_ts_bundle output entry)
    cmake_parse_arguments(ARG "" "ROOT" "DEPENDS" ${ARGN})
    get_filename_component(entry_dir "${entry}" DIRECTORY)
    set(root "${entry_dir}")
    if(ARG_ROOT)
        set(root "${ARG_ROOT}")
    endif()
    file(GLOB_RECURSE local_sources CONFIGURE_DEPENDS "${root}/*.ts" "${root}/*.tsx")
    list(FILTER local_sources EXCLUDE REGEX "\\.check\\.tsx?$")
    set(tsx_sources ${local_sources})
    list(FILTER tsx_sources INCLUDE REGEX "\\.tsx$")
    set(bundle_entry "${entry}")
    set(mirror_depends)
    if(tsx_sources)
        get_filename_component(name "${output}" NAME_WE)
        set(mirror "${CMAKE_CURRENT_BINARY_DIR}/tsx/${name}")
        file(RELATIVE_PATH rel "${root}" "${entry}")
        set(bundle_entry "${mirror}/${rel}")
        set(stamp "${mirror}.stamp")
        add_custom_command(
            OUTPUT "${stamp}"
            COMMAND axe-tsx --root "${root}" --out "${mirror}" ${local_sources}
            COMMAND "${CMAKE_COMMAND}" -E touch "${stamp}"
            DEPENDS axe-tsx ${local_sources}
            COMMENT "Compiling the TSX of ${name}"
            VERBATIM)
        set(mirror_depends "${stamp}")
    endif()
    add_custom_command(
        OUTPUT "${output}"
        COMMAND "${AXE_ESBUILD}" "${bundle_entry}" --bundle --format=esm --platform=neutral --target=es2023
                --keep-names --external:axe:host --external:axe:test --external:axe:stall --log-level=warning
                "--tsconfig=${AXE_TS_DIR}/tsconfig.json"
                "--define:AXE_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\"" "--outfile=${output}"
        DEPENDS "${entry}" ${local_sources} ${mirror_depends} ${AXE_TS_DEPENDS} ${ARG_DEPENDS}
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
