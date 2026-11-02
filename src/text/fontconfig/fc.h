// Font discovery on Linux. This module is the only code that includes fontconfig.h; other
// platforms get their own backend (DirectWrite, CoreText) behind the same three calls.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ui/result.h"

typedef struct ui_fc ui_fc;

// `system` loads the system configuration; otherwise the configuration knows only
// `extra_dirs` (deterministic, for tests). Extra directories are added in both cases.
ui_result ui_fc_create(bool system, const char *const *extra_dirs, uint32_t dir_count,
                       ui_fc **out);
void ui_fc_destroy(ui_fc *fc);

typedef struct ui_fc_font {
    char path[1024];
    uint32_t index;
} ui_fc_font;

// The best face of `family` (or a generic family: sans-serif, serif, monospace) for the
// weight (1..1000, CSS scale) and italic flag. False if Fontconfig has no such family; a
// generic family always resolves when any font is installed.
bool ui_fc_match(ui_fc *fc, const char *family, uint16_t weight, bool italic, ui_fc_font *out);

// A face that covers every code point in cps, preferring sans-serif at the weight and
// style. False if no installed font covers them.
bool ui_fc_fallback(ui_fc *fc, const uint32_t *cps, uint32_t count, uint16_t weight,
                    bool italic, ui_fc_font *out);
