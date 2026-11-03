// The migration proof's checker (tests/wire/proof.md): reads every buffer given and fails on
// the first one the pre-IDL code and the generated codec decode differently.
//
//   wire_proof --commands <files...> --events <files...> --fuzz-commands <files...>
//              --fuzz-set-text <files...>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "proof_dump.h"
#include "proof_inputs.h"

static uint8_t *slurp(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)n + 1);
    if (data && fread(data, 1, (size_t)n, f) != (size_t)n) {
        free(data);
        data = NULL;
    }
    fclose(f);
    *size = (size_t)n;
    return data;
}

static const char *dump_dir; // --dump-to: write each buffer's legacy dump there

static int compare(const char *path, const char *what, const uint8_t *data, size_t size, bool events) {
    proof_text a = {0}, b = {0};
    (events ? proof_dump_events : proof_dump_commands)(&a, &b, data, size);
    int bad = a.size != b.size || memcmp(a.data, b.data, a.size);
    if (dump_dir) {
        const char *base = strrchr(path, '/');
        char out[4096];
        snprintf(out, sizeof out, "%s/%s.txt", dump_dir, base ? base + 1 : path);
        FILE *f = fopen(out, "wb");
        if (f) {
            fwrite(a.data, 1, a.size, f);
            fclose(f);
        }
    }
    if (bad) {
        fprintf(stderr, "%s (%s): decoded differently\n--- legacy\n%s--- generated\n%s", path, what, a.data, b.data);
    }
    proof_text_free(&a);
    proof_text_free(&b);
    return bad;
}

int main(int argc, char **argv) {
    enum { NONE, COMMANDS, EVENTS, FUZZ_COMMANDS, FUZZ_SET_TEXT } mode = NONE;
    unsigned files = 0, failures = 0, buffers = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dump-to") && i + 1 < argc) dump_dir = argv[++i];
        else if (!strcmp(argv[i], "--commands")) mode = COMMANDS;
        else if (!strcmp(argv[i], "--events")) mode = EVENTS;
        else if (!strcmp(argv[i], "--fuzz-commands")) mode = FUZZ_COMMANDS;
        else if (!strcmp(argv[i], "--fuzz-set-text")) mode = FUZZ_SET_TEXT;
        else {
            size_t size;
            uint8_t *data = slurp(argv[i], &size);
            if (!data || mode == NONE) {
                fprintf(stderr, "%s: can't read, or no mode given\n", argv[i]);
                return 2;
            }
            files++;
            if (mode == COMMANDS || mode == EVENTS) {
                failures += (unsigned)compare(argv[i], "buffer", data, size, mode == EVENTS);
                buffers++;
            } else if (mode == FUZZ_SET_TEXT) {
                size_t n;
                uint8_t *buf = proof_set_text_buffer(data, size, &n);
                if (buf) failures += (unsigned)compare(argv[i], "set_text seed", buf, n, false), buffers++;
                free(buf);
            } else if (size >= 4) {
                // As fuzz_commands: the ops after the first byte, whole and split at the op the
                // first byte names, and the same bytes read as an event buffer.
                uint32_t split = data[0] >> 3;
                const uint8_t *ops = data + 1;
                size_t body = (size - 1) & ~(size_t)3, at = 0;
                uint32_t count = proof_count_ops(ops, body), want = (uint32_t)((uint64_t)count * split / 32);
                for (uint32_t k = 0; k < want && at + UI_WIRE_OP_HEADER_SIZE <= body; k++) {
                    uint16_t op_size = ui_wire_get_u16(ops + at + 2);
                    if (op_size < UI_WIRE_OP_HEADER_SIZE || op_size % 4 || op_size > body - at) break;
                    at += op_size;
                }
                const struct { size_t from, to; } parts[] = {{0, body}, {0, at}, {at, body}};
                for (size_t k = 0; k < 3; k++) {
                    size_t n;
                    uint8_t *buf = proof_make_buffer(UI_WIRE_MAGIC_CMD, ops + parts[k].from, parts[k].to - parts[k].from, &n);
                    failures += (unsigned)compare(argv[i], "commands seed", buf, n, false);
                    free(buf);
                    buf = proof_make_buffer(UI_WIRE_MAGIC_EVENT, ops + parts[k].from, parts[k].to - parts[k].from, &n);
                    failures += (unsigned)compare(argv[i], "seed as events", buf, n, true);
                    free(buf);
                    buffers += 2;
                }
            }
            free(data);
        }
    }
    printf("wire proof: %u files, %u buffers, %u decoded differently\n", files, buffers, failures);
    return failures ? 1 : 0;
}
