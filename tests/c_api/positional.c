// A positional initializer of a descriptor that gained fields since it was written (a style
// block from before token scopes). Unsupported (docs/c-api.md): CTest expects compiling it
// with the warnings of designated.c to fail, as -Wmissing-field-initializers flags it.
#include "ui/ui.h"

ui_result positional(ui_cmd_writer *w, const ui_style_decl *decls);

ui_result positional(ui_cmd_writer *w, const ui_style_decl *decls) {
    return ui_cmd_define_style(w, 1, &(ui_style_block_desc){0, decls, 1}, 1);
}
