// Decoding set_text: one op plus the append_text ops that continue it become an immutable
// text source. Pure (no scene state), so the workers that prepare command buffers run it
// as well as the core when it applies a buffer that was not prepared.
#pragma once

#include "text/paragraph.h"
#include "wire/wire.h"

typedef struct ui_text_op {
    ui_handle node;
    ui_text_source *source; // NULL when the op is dropped
    const char *why;        // drop reason when source is NULL
    bool out_of_memory;     // the drop was for lack of memory (logged as an error)
    uint32_t replaced;      // ill-formed UTF-8 subparts replaced with U+FFFD
    uint32_t continuations; // append_text ops consumed from the reader
} ui_text_op;

// Decodes the set_text `op` (validated framing) and consumes its continuations from `r`
// (positioned after `op`). Every check that fails drops the op and what it consumed.
void ui_text_op_decode(const ui_wire_op *op, ui_wire_reader *r, ui_text_op *out);
void ui_text_op_free(ui_text_op *t);
