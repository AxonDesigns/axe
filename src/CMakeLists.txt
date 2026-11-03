include(${PROJECT_SOURCE_DIR}/cmake/WaylandProtocols.cmake)
include(${PROJECT_SOURCE_DIR}/cmake/GenWebGPUChecked.cmake)

find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND REQUIRED IMPORTED_TARGET wayland-client>=1.20)
pkg_check_modules(XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon>=1.0)
pkg_check_modules(WAYLAND_PROTOCOLS REQUIRED wayland-protocols>=1.31)
pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig>=2.13)

add_library(ui STATIC
    runtime/actor.c
    runtime/app.c
    runtime/common.c
    runtime/handle_table.c
    runtime/jobs.c
    runtime/logic_actor.c
    runtime/queue.c
    runtime/sched_single.c
    runtime/sched_threaded.c
    wire/wire.c
    wire/gen/wire_gen.c
    core/clock.c
    core/core.c
    core/display_list.c
    layout/flex.c
    core/prepare.c
    core/preview.c
    core/scene.c
    core/text_op.c
    core/spring.c
    hooks/hooks.c
    image/image_codec.c
    hooks/trace_printer.c
    platform/headless/headless.c
    platform/wayland/pacing.c
    platform/wayland/wayland.c
    render/render.c
    style/style.c
    text/atlas.c
    text/font_registry.c
    text/glyph_cache.c
    text/hittest.c
    text/paragraph.c
    text/raster.c
    text/shape.c
    unicode/bidi.c
    unicode/grapheme.c
    unicode/linebreak.c
    unicode/script.c
    unicode/ucd.c
    unicode/ucd_tables.c
    unicode/utf8.c)
target_include_directories(ui
    PUBLIC ${PROJECT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# POSIX 2008 + BSD interfaces (clock_gettime, poll) under strict -std=c23.
target_compile_definitions(ui PRIVATE UI_HOOKS_ENABLED=$<BOOL:${UI_HOOKS}> _DEFAULT_SOURCE)
target_link_libraries(ui PRIVATE m PkgConfig::WAYLAND PkgConfig::XKBCOMMON)
ui_target_warnings(ui)

# One internal module per dependency: only text/hb includes hb.h and only text/fontconfig
# includes fontconfig.h; neither type appears in any other header.
target_sources(ui PRIVATE text/hb/hb_font.c text/fontconfig/fc.c)
target_link_libraries(ui PRIVATE harfbuzz PkgConfig::FONTCONFIG)

# The render module is the only code that sees webgpu.h.
ui_generate_webgpu_checked("${CMAKE_BINARY_DIR}/gen/render/webgpu_checked.h" ${UI_WEBGPU_HEADERS})
set_source_files_properties(render/render.c PROPERTIES
    INCLUDE_DIRECTORIES "${CMAKE_BINARY_DIR}/gen/render"
    COMPILE_DEFINITIONS UI_WEBGPU_WGPU_NATIVE)
target_link_libraries(ui PRIVATE ui_webgpu)

# Protocol glue is generated code: compiled without our warning flags.
add_library(ui_wayland_protocols STATIC)
foreach(xml IN ITEMS
        stable/xdg-shell/xdg-shell.xml
        stable/viewporter/viewporter.xml
        stable/presentation-time/presentation-time.xml
        staging/fractional-scale/fractional-scale-v1.xml
        unstable/xdg-decoration/xdg-decoration-unstable-v1.xml)
    ui_wayland_protocol(ui_wayland_protocols ${xml})
endforeach()
target_link_libraries(ui_wayland_protocols PUBLIC PkgConfig::WAYLAND)
target_link_libraries(ui PRIVATE ui_wayland_protocols)
