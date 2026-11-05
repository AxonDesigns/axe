# The Luau binding (README.md): a second language behind the same wire and host interface. A
# library of its own, so `ui` doesn't link Luau.
set(_lib ${CMAKE_CURRENT_SOURCE_DIR}/lib)
set(AXE_LUAU_LIB ${_lib}/axe.luau ${_lib}/wire/runtime.luau ${_lib}/wire/reader.luau ${_lib}/wire/gen/ids.luau
    ${_lib}/wire/gen/commands.luau ${_lib}/wire/gen/events.luau ${_lib}/wire/gen/styles.luau)
add_library(axe_luau STATIC host.c)
set_source_files_properties(host.c PROPERTIES OBJECT_DEPENDS "${AXE_LUAU_LIB}")
target_include_directories(axe_luau PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(axe_luau PUBLIC ui PRIVATE luau)
target_compile_definitions(axe_luau PRIVATE _DEFAULT_SOURCE)
ui_target_warnings(axe_luau)

# axe_luau_app(<name> ENTRY <main.luau> [TITLE t]): axe-run with this Luau app built in.
function(axe_luau_app name)
    cmake_parse_arguments(ARG "" "ENTRY;TITLE" "" ${ARGN})
    add_executable(${name} "${PROJECT_SOURCE_DIR}/tools/axe-run/main.c")
    target_link_libraries(${name} PRIVATE axe_luau)
    get_filename_component(entry_name "${ARG_ENTRY}" NAME)
    target_compile_definitions(${name} PRIVATE _DEFAULT_SOURCE AXE_RUN_LUAU "AXE_RUN_NAME=\"${entry_name}\""
                               "AXE_RUN_EMBED=\"${ARG_ENTRY}\"")
    if(ARG_TITLE)
        target_compile_definitions(${name} PRIVATE "AXE_RUN_TITLE=\"${ARG_TITLE}\"")
    endif()
    set_source_files_properties("${PROJECT_SOURCE_DIR}/tools/axe-run/main.c" TARGET_DIRECTORY ${name}
                                PROPERTIES OBJECT_DEPENDS "${ARG_ENTRY}")
    ui_target_warnings(${name})
endfunction()
