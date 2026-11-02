add_library(ui STATIC runtime/placeholder.c)
target_link_libraries(ui PRIVATE ui_webgpu)
