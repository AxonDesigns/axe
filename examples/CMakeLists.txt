# The tests run the examples: in sanitizer builds they keep shared objects loaded as the tests
# do (tests/keep_loaded.c).
if("address" IN_LIST UI_SANITIZE)
    link_libraries(ui_keep_loaded)
endif()

# The demo logic is a library so the headless test drives exactly the same logic.
add_library(spring_rect_logic STATIC spring_rect/demo_logic.c)
target_include_directories(spring_rect_logic PUBLIC spring_rect)
target_link_libraries(spring_rect_logic PUBLIC ui)
ui_target_warnings(spring_rect_logic)

add_executable(spring_rect spring_rect/main.c)
target_link_libraries(spring_rect PRIVATE spring_rect_logic)
ui_target_warnings(spring_rect)

add_executable(text_paragraph text_paragraph/main.c)
target_compile_definitions(text_paragraph PRIVATE AXE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_link_libraries(text_paragraph PRIVATE ui)
ui_target_warnings(text_paragraph)

add_executable(layout_cards layout_cards/main.c)
target_compile_definitions(layout_cards PRIVATE AXE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_link_libraries(layout_cards PRIVATE ui)
ui_target_warnings(layout_cards)

# The TypeScript ports (docs/typescript.md): the same apps on @axe/core, run by axe-run.
axe_ts_app(spring_rect_ts ENTRY ${CMAKE_CURRENT_SOURCE_DIR}/spring_rect/main.ts
           TITLE "Axe — spring_rect (TypeScript)")
axe_ts_app(layout_cards_ts ENTRY ${CMAKE_CURRENT_SOURCE_DIR}/layout_cards/main.ts
           WIDTH 980 HEIGHT 640 SCALE 150 TITLE "Axe — layout_cards (TypeScript)")
# And in TSX, with signals and components (docs/ui.md).
axe_ts_app(layout_cards_tsx ENTRY ${CMAKE_CURRENT_SOURCE_DIR}/layout_cards/main.tsx
           WIDTH 980 HEIGHT 640 SCALE 150 TITLE "Axe — layout_cards (TSX)")

# And in Luau: the second binding (bindings/luau).
if(TARGET axe_luau)
    axe_luau_app(spring_rect_luau ENTRY ${CMAKE_CURRENT_SOURCE_DIR}/spring_rect/main.luau
                 TITLE "Axe — spring_rect (Luau)")
endif()
