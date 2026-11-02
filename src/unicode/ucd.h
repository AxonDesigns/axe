// Unicode character properties from the generated tables (src/unicode/ucd_tables.c). The
// UCD version is UI_UCD_VERSION and matches the one the pinned HarfBuzz ships.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "unicode/ucd_tables.h"

#define UI_MAX_CODE_POINT 0x10FFFFu

// Properties of `cp`. Values above U+10FFFF are looked up as U+FFFD.
static inline const ui_ucd_record *ui_ucd(uint32_t cp) {
    if (cp > UI_MAX_CODE_POINT) cp = 0xFFFD;
    uint32_t block = ui_ucd_stage1[cp >> UI_UCD_SHIFT];
    return &ui_ucd_records[ui_ucd_stage2[(block << UI_UCD_SHIFT) |
                                         (cp & ((1u << UI_UCD_SHIFT) - 1))]];
}

static inline ui_incb ui_ucd_incb(const ui_ucd_record *r) {
    return (ui_incb)((r->flags >> UI_UCD_INCB_SHIFT) & 3u);
}

// Bidi_Paired_Bracket of `cp`, or false if it is not a paired bracket.
bool ui_ucd_paired_bracket(uint32_t cp, uint32_t *pair, bool *open);

// Script_Extensions of a record: a pointer to `*count` ui_script values. For characters
// without an explicit list this is the record's own Script.
const uint8_t *ui_ucd_script_extensions(const ui_ucd_record *r, uint32_t *count);

// ISO 15924 tag of a script, big-endian ('Latn' = 0x4C61746E), as HarfBuzz's hb_script_t.
static inline uint32_t ui_script_tag(ui_script script) { return ui_ucd_script_tags[script]; }
