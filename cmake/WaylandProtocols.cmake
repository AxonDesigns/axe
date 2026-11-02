# ui_wayland_protocol(<target> <xml>): runs wayland-scanner at build time and adds the
# generated client header and glue code to <target>.
find_package(PkgConfig REQUIRED)
pkg_get_variable(UI_WAYLAND_SCANNER wayland-scanner wayland_scanner)
pkg_get_variable(UI_WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
if(NOT UI_WAYLAND_SCANNER OR NOT UI_WAYLAND_PROTOCOLS_DIR)
    message(FATAL_ERROR "wayland-scanner and wayland-protocols are required")
endif()

function(ui_wayland_protocol target xml)
    get_filename_component(name "${xml}" NAME_WE)
    set(out "${CMAKE_BINARY_DIR}/gen/wayland")
    add_custom_command(
        OUTPUT "${out}/${name}-client-protocol.h" "${out}/${name}-protocol.c"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${out}"
        COMMAND "${UI_WAYLAND_SCANNER}" client-header "${UI_WAYLAND_PROTOCOLS_DIR}/${xml}"
                "${out}/${name}-client-protocol.h"
        COMMAND "${UI_WAYLAND_SCANNER}" private-code "${UI_WAYLAND_PROTOCOLS_DIR}/${xml}"
                "${out}/${name}-protocol.c"
        DEPENDS "${UI_WAYLAND_PROTOCOLS_DIR}/${xml}"
        COMMENT "wayland-scanner ${name}"
        VERBATIM)
    target_sources(${target} PRIVATE "${out}/${name}-client-protocol.h" "${out}/${name}-protocol.c")
    target_include_directories(${target} SYSTEM PUBLIC "${out}")
endfunction()
