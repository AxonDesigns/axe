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
