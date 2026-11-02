add_library(ui STATIC
    runtime/actor.c
    runtime/common.c
    runtime/handle_table.c
    runtime/queue.c
    wire/wire.c
    core/clock.c
    core/display_list.c
    core/scene.c
    core/spring.c)
target_include_directories(ui
    PUBLIC ${PROJECT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# POSIX 2008 + BSD interfaces (clock_gettime, poll) under strict -std=c23.
target_compile_definitions(ui PRIVATE _DEFAULT_SOURCE)
target_link_libraries(ui PRIVATE m)
ui_target_warnings(ui)
