// Turns fuzz input into code points: 3 bytes each, folded into U+0000..U+10FFFF (skipping
// surrogates), so every property class is reachable.
#pragma once

#include <stddef.h>
#include <stdint.h>

static inline size_t fuzz_cps(const uint8_t *data, size_t size, uint32_t *cps, size_t cap) {
    size_t n = 0;
    for (size_t i = 0; i + 3 <= size && n < cap; i += 3) {
        uint32_t v = (uint32_t)data[i] << 16 | (uint32_t)data[i + 1] << 8 | data[i + 2];
        // Bias toward the BMP and the common blocks, where the interesting classes are.
        uint32_t cp = (v & 0x800000) ? v % 0x110000u : v % 0x3000u;
        if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
        cps[n++] = cp;
    }
    return n;
}
