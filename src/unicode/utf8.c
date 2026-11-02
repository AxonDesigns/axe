#include "unicode/utf8.h"

#include <stdlib.h>
#include <string.h>

// Well-formed sequences (Unicode Table 3-7). The second byte's range depends on the lead
// byte; every later byte is 80..BF.
int32_t ui_utf8_decode(const uint8_t *s, size_t n, uint32_t *len) {
    uint8_t c = s[0];
    if (c < 0x80) {
        *len = 1;
        return c;
    }
    uint32_t need;
    uint8_t lo = 0x80, hi = 0xBF;
    uint32_t cp;
    if (c >= 0xC2 && c <= 0xDF) {
        need = 1;
        cp = c & 0x1Fu;
    } else if (c >= 0xE0 && c <= 0xEF) {
        need = 2;
        cp = c & 0x0Fu;
        if (c == 0xE0) lo = 0xA0;
        else if (c == 0xED) hi = 0x9F;
    } else if (c >= 0xF0 && c <= 0xF4) {
        need = 3;
        cp = c & 0x07u;
        if (c == 0xF0) lo = 0x90;
        else if (c == 0xF4) hi = 0x8F;
    } else {
        *len = 1; // 80..C1, F5..FF can never start a sequence
        return -1;
    }
    for (uint32_t k = 1; k <= need; k++) {
        if (k >= n || s[k] < lo || s[k] > hi) {
            *len = k; // the lead plus every continuation byte that still fit
            return -1;
        }
        cp = (cp << 6) | (s[k] & 0x3Fu);
        lo = 0x80;
        hi = 0xBF;
    }
    *len = need + 1;
    return (int32_t)cp;
}

uint32_t ui_utf8_encode(uint32_t cp, uint8_t out[4]) {
    if (cp < 0x80) {
        out[0] = (uint8_t)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (uint8_t)(0xC0 | (cp >> 6));
        out[1] = (uint8_t)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (uint8_t)(0xE0 | (cp >> 12));
        out[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (uint8_t)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (uint8_t)(0xF0 | (cp >> 18));
    out[1] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (uint8_t)(0x80 | (cp & 0x3F));
    return 4;
}

bool ui_utf8_valid(const uint8_t *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        if (s[i] < 0x80) {
            i++;
            continue;
        }
        uint32_t len;
        if (ui_utf8_decode(s + i, n - i, &len) < 0) return false;
        i += len;
    }
    return true;
}

ui_result ui_utf8_sanitize(const uint8_t *s, size_t n, uint8_t **out, size_t *out_n,
                           uint32_t *replaced) {
    // Every replaced subpart is >= 1 byte and becomes 3, so 3n bounds the output.
    size_t first_bad = 0;
    uint32_t count = 0;
    size_t i = 0;
    bool valid = true;
    while (i < n) {
        uint32_t len;
        if (s[i] < 0x80) {
            i++;
            continue;
        }
        if (ui_utf8_decode(s + i, n - i, &len) < 0) {
            first_bad = i;
            valid = false;
            break;
        }
        i += len;
    }
    uint8_t *buf = malloc(valid ? n + 1 : n * 3 + 1);
    if (!buf) return UI_ERR_OUT_OF_MEMORY;
    if (valid) {
        if (n) memcpy(buf, s, n);
        buf[n] = 0;
        *out = buf;
        *out_n = n;
        *replaced = 0;
        return UI_OK;
    }
    memcpy(buf, s, first_bad);
    size_t o = first_bad;
    for (i = first_bad; i < n;) {
        uint32_t len;
        if (ui_utf8_decode(s + i, n - i, &len) < 0) {
            buf[o++] = 0xEF;
            buf[o++] = 0xBF;
            buf[o++] = 0xBD;
            count++;
        } else {
            memcpy(buf + o, s + i, len);
            o += len;
        }
        i += len;
    }
    buf[o] = 0;
    *out = buf;
    *out_n = o;
    *replaced = count;
    return UI_OK;
}
