// Watching an app's sources (docs/ui.md, "Hot reload"): a small interface, so a platform without
// inotify implements these four functions.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ui/result.h"

#define UI_WATCH_DEBOUNCE_MS 15

typedef struct ui_watch ui_watch;

// Watches `root` and the directories below it. Ownership: ui_watch_close.
ui_result ui_watch_open(const char *root, ui_watch **out);

typedef struct ui_watch_batch {
    char **paths;   // relative to the root, sorted and distinct; free with ui_watch_batch_free
    uint32_t count;
    uint64_t event_ns; // CLOCK_MONOTONIC when the first event was read
} ui_watch_batch;

// Waits up to `timeout_ms` (negative: forever) for source files to change, then for the burst to
// end, and returns what changed. What a path became (there, or gone) is up to the caller: saves
// arrive in many shapes (a write in place, a rename over the file, a delete and a new file), and
// all of them come down to "look at it now". False: nothing changed, or ui_watch_interrupt.
bool ui_watch_next(ui_watch *w, int timeout_ms, ui_watch_batch *out);
void ui_watch_batch_free(ui_watch_batch *b);
// Makes a waiting ui_watch_next return. Thread: any.
void ui_watch_interrupt(ui_watch *w);
void ui_watch_close(ui_watch *w);
