// UTF-8 validation and replacement: the output is always valid, valid input passes through
// unchanged, and the replacement count matches a decode loop over maximal subparts.
#include <stdlib.h>
#include <string.h>

#include "unicode/utf8.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    uint8_t *out;
    size_t out_n;
    uint32_t replaced;
    if (ui_utf8_sanitize(data, size, &out, &out_n, &replaced) != UI_OK) return 0;
    if (!ui_utf8_valid(out, out_n)) abort();
    uint32_t bad = 0;
    size_t expected_len = 0;
    for (size_t i = 0; i < size;) {
        uint32_t len;
        if (ui_utf8_decode(data + i, size - i, &len) < 0) {
            bad++;
            expected_len += 3;
        } else {
            expected_len += len;
        }
        if (len == 0) abort(); // decoding must always make progress
        i += len;
    }
    if (bad != replaced || expected_len != out_n) abort();
    if (!replaced && (out_n != size || memcmp(out, data, size))) abort();
    // Valid output decodes and re-encodes to itself.
    for (size_t i = 0; i < out_n;) {
        size_t at = i;
        uint32_t cp = ui_utf8_next(out, &i);
        uint8_t enc[4];
        uint32_t n = ui_utf8_encode(cp, enc);
        if (n != i - at || memcmp(enc, out + at, n)) abort();
    }
    free(out);
    return 0;
}
