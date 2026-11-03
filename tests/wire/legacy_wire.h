// The pre-IDL encoders and event reader (tests/wire/legacy_wire.c), for the migration proof.
#pragma once

#include "wire/wire.h"

typedef struct legacy_writer {
    ui_wire_buf buf;
} legacy_writer;

ui_result legacy_writer_create(legacy_writer **out);
void legacy_writer_destroy(legacy_writer *writer);
uint32_t legacy_writer_op_count(const legacy_writer *writer);
ui_result legacy_writer_take(legacy_writer *w, uint8_t **out_bytes, size_t *out_size);
ui_result legacy_cmd_create_node(legacy_writer *w, ui_handle node, ui_handle parent);
ui_result legacy_cmd_destroy_node(legacy_writer *w, ui_handle node);
ui_result legacy_cmd_move_node(legacy_writer *w, ui_handle node, ui_handle parent, ui_handle before);
ui_result legacy_cmd_request_layout(legacy_writer *w, ui_handle node, uint32_t request_id);
ui_result legacy_cmd_set_rect(legacy_writer *w, ui_handle node, float x, float y, float width, float height);
ui_result legacy_cmd_set_corner_radius(legacy_writer *w, ui_handle node, float radius);
ui_result legacy_cmd_set_color(legacy_writer *w, ui_handle node, float r, float g, float b, float a);
ui_result legacy_cmd_set_clip(legacy_writer *w, ui_handle node, bool clip);
ui_result legacy_cmd_start_spring(legacy_writer *w, ui_handle node, ui_property property, const float *target,
                                  float stiffness, float damping);
ui_result legacy_cmd_set_timer(legacy_writer *w, uint32_t id, uint64_t deadline_ns);
ui_result legacy_cmd_cancel_timer(legacy_writer *w, uint32_t id);
ui_result legacy_cmd_set_text(legacy_writer *w, ui_handle node, const ui_text_desc *d);
ui_result legacy_cmd_load_font(legacy_writer *w, uint32_t id, const char *path);
ui_result legacy_cmd_define_style(legacy_writer *w, uint32_t id, const ui_style_block_desc *blocks,
                                  uint32_t block_count);
ui_result legacy_cmd_destroy_style(legacy_writer *w, uint32_t id);
ui_result legacy_cmd_set_styles(legacy_writer *w, ui_handle node, const uint32_t *ids, uint32_t count);
ui_result legacy_cmd_set_props(legacy_writer *w, ui_handle node, const ui_style_decl *decls, uint32_t count);
ui_result legacy_cmd_set_token(legacy_writer *w, uint32_t id, ui_value value);
ui_result legacy_cmd_set_state(legacy_writer *w, ui_handle node, uint32_t mask, uint32_t value);

ui_result legacy_event_frame_committed(ui_wire_buf *b, ui_handle surface, uint64_t frame_id,
                                       uint64_t predicted_present_ns);
ui_result legacy_event_surface_resized(ui_wire_buf *b, ui_handle surface, float width, float height,
                                       uint32_t scale_120);
ui_result legacy_event_surface_close(ui_wire_buf *b, ui_handle surface);
ui_result legacy_event_font_loaded(ui_wire_buf *b, uint32_t id, ui_result result);
ui_result legacy_event_layout_measured(ui_wire_buf *b, uint32_t request_id, ui_handle node, ui_handle surface,
                                       ui_result result, const float rects[4][4]);
ui_result legacy_event_timer_fired(ui_wire_buf *b, uint32_t id, uint64_t deadline_ns, uint64_t now_ns);
ui_result legacy_event_reader_init(ui_event_reader *reader, const void *data, size_t size);
ui_result legacy_event_next(ui_event_reader *reader, ui_event *out);
