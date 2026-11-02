// UTF-8 validation, decoding and sanitizing. Text inside the core is always valid UTF-8:
// invalid input is replaced at the boundary (the wire) with U+FFFD, one per maximal
// subpart of an ill-formed sequence (Unicode §3.9, "U+FFFD Substitution of Maximal
// Subparts"; the same policy as the WHATWG Encoding Standard).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ui/result.h"

// Decodes the sequence at s[0..n) (n >= 1). Returns the code point and sets *len to its
// length, or returns -1 with *len set to the length of the maximal subpart (>= 1) that has
// to be replaced.
int32_t ui_utf8_decode(const uint8_t *s, size_t n, uint32_t *len);

// Decodes the next code point of *valid* UTF-8 and advances *i.
static inline uint32_t ui_utf8_next(const uint8_t *s, size_t *i) {
    uint32_t c = s[*i];
    if (c < 0x80) {
        *i += 1;
        return c;
    }
    if (c < 0xE0) {
        uint32_t cp = ((c & 0x1Fu) << 6) | (s[*i + 1] & 0x3Fu);
        *i += 2;
        return cp;
    }
    if (c < 0xF0) {
        uint32_t cp = ((c & 0x0Fu) << 12) | ((s[*i + 1] & 0x3Fu) << 6) | (s[*i + 2] & 0x3Fu);
        *i += 3;
        return cp;
    }
    uint32_t cp = ((c & 0x07u) << 18) | ((s[*i + 1] & 0x3Fu) << 12) |
                  ((s[*i + 2] & 0x3Fu) << 6) | (s[*i + 3] & 0x3Fu);
    *i += 4;
    return cp;
}

// Length in bytes of the valid sequence starting with lead byte `c`.
static inline uint32_t ui_utf8_seq_len(uint8_t c) {
    return c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
}

// Encodes `cp` (a scalar value) into out; returns the length (1..4).
uint32_t ui_utf8_encode(uint32_t cp, uint8_t out[4]);

bool ui_utf8_valid(const uint8_t *s, size_t n);

// Copies s[0..n) into a new malloc'd buffer with every maximal subpart of an ill-formed
// sequence replaced by U+FFFD. *replaced receives the number of replacements. The result is
// NUL-terminated (not counted in *out_n) so it can be logged. Fails only on allocation.
ui_result ui_utf8_sanitize(const uint8_t *s, size_t n, uint8_t **out, size_t *out_n,
                           uint32_t *replaced);
