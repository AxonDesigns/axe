// Canonical dumps of wire buffers (tests/wire/wire_dump.c), for test_wire_fixtures.
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct wire_text {
    char *data;
    size_t size, capacity;
} wire_text;

void wire_dump_commands(wire_text *t, const uint8_t *data, size_t size);
void wire_dump_events(wire_text *t, const uint8_t *data, size_t size);
void wire_text_free(wire_text *t);
