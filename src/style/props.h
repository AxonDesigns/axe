// Style properties and value kinds on the wire (docs/styles.md): which kinds each property
// accepts and the ranges values must be in. Shared by the command writer, which rejects
// invalid declarations, and the core, which drops ops that carry them.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ui/wire.h"

// Size of an entry with this kind, in 4-byte words including its 4-byte header (the
// smallest, for strings); 0 for a kind this version doesn't know (a reader skips such
// entries by their own word count).
uint32_t ui_style_kind_words(uint8_t kind);

// Size of an entry carrying this value (strings vary), in words including the header.
uint32_t ui_style_value_words(const ui_value *v);

// A string value: 1 to UI_STYLE_STRING_MAX bytes of UTF-8 without NUL.
bool ui_style_string_valid(const ui_value *v);

// True if `prop` is a property (or a transition of one) this version knows.
bool ui_style_prop_known(uint16_t prop);

// True if a declaration of `prop` with this value is valid: the property accepts the kind
// and the value is in range. Token references are accepted for every value property (their
// value is checked when resolved). `override` allows UI_VALUE_UNSET.
bool ui_style_value_valid(uint16_t prop, const ui_value *value, bool override);

// True if a token may hold this value: any kind but unset and spring, finite.
bool ui_style_token_value_valid(const ui_value *value);
