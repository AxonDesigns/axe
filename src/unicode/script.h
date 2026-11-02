// Script itemization (UAX #24): resolves Common and Inherited characters to the script of
// their run, so text can be split into single-script runs for shaping.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "unicode/ucd_tables.h"

// Writes one ui_script per code point; equal neighbours form a run.
//  - Inherited characters (combining marks) join the run before them.
//  - Common characters join the run they are in; leading ones join the first real script.
//  - Script_Extensions count: a character belonging to several scripts continues any run
//    whose script is one of them.
//  - A closing bracket takes the script of its matching opening bracket, so "(" and ")"
//    around another script's text agree.
// Text with no real script resolves to UI_SCRIPT_ZYYY (Common).
void ui_script_runs(const uint32_t *cps, size_t n, uint8_t *out);
