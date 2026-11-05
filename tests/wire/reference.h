// The encoder's reference cases (reference.c).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The command buffer of case `name` (malloc'd); false if there's no such case.
bool ui_test_reference_commands(const char *name, uint8_t **bytes, size_t *size);
// An event buffer of every event, then one from a newer core (malloc'd).
uint8_t *ui_test_reference_events(size_t *size);
