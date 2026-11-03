// The migration proof's fuzzer (tests/wire/proof.md): arbitrary ops, as fuzz_commands builds
// them, must decode the same with the pre-IDL code and the generated codec, as commands and as
// events. Built with UI_WIRE_PROOF only.
#include <stdlib.h>
#include <string.h>

#include "proof_dump.h"
#include "proof_inputs.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void same(const uint8_t *buf, size_t n, bool events) {
    proof_text a = {0}, b = {0};
    (events ? proof_dump_events : proof_dump_commands)(&a, &b, buf, n);
    if (a.size != b.size || memcmp(a.data, b.data, a.size)) __builtin_trap();
    proof_text_free(&a);
    proof_text_free(&b);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 4 || size > (1u << 20)) return 0;
    size_t body = (size - 1) & ~(size_t)3, n;
    for (int events = 0; events < 2; events++) {
        uint8_t *buf = proof_make_buffer(events ? UI_WIRE_MAGIC_EVENT : UI_WIRE_MAGIC_CMD, data + 1, body, &n);
        same(buf, n, events);
        free(buf);
    }
    uint8_t *buf = proof_set_text_buffer(data, size, &n);
    if (buf) same(buf, n, false);
    free(buf);
    return 0;
}
