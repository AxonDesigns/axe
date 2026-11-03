// Canonical dumps for the migration proof (tests/wire/proof.md): the same buffer read by the
// pre-IDL code and by the generated codec. Equal dumps mean equal decoding.
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct proof_text {
    char *data;
    size_t size, capacity;
} proof_text;

void proof_dump_commands(proof_text *legacy, proof_text *generated, const uint8_t *data, size_t size);
void proof_dump_events(proof_text *legacy, proof_text *generated, const uint8_t *data, size_t size);
void proof_text_free(proof_text *t);
