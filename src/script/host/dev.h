// Hot reload's host side (docs/ui.md, "Hot reload"), only in AXE_DEV builds.
//
// The dev tools (axe-run's watcher, the test runner) rebuild changed files and post batches to
// a channel, from any thread. The host takes them at the start of a turn, keeps each file's
// newest code in a module table, and serves it to the engine under versioned names
// (app:/card.tsx?v=3), since an engine can't replace a module. @axe/ui's hot runtime decides
// what to import; everything it imports is staged until it commits or reverts.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "script/engine.h"

typedef struct ui_dev_file {
    char *path;  // relative to the app's root, malloc'd
    char *code;  // the module's JS, malloc'd; NULL when deleted or failed
    size_t size;
    char *error; // the build's message, malloc'd, if the build failed
    bool deleted;
} ui_dev_file;

typedef struct ui_dev_batch {
    ui_dev_file *files; // malloc'd
    uint32_t count;
    uint64_t event_ns;  // CLOCK_MONOTONIC of the first file event behind it; 0 for the first
    struct ui_dev_batch *next;
} ui_dev_batch;

typedef struct ui_dev_channel ui_dev_channel;

typedef struct ui_dev_desc {
    const char *entry;    // the entry module, relative to the root
    const char *packages; // the directory of the prebuilt @axe/* modules
} ui_dev_desc;

// Ownership: destroy after the app. The channel must outlive the app.
ui_result ui_dev_channel_create(const ui_dev_desc *desc, ui_dev_channel **out);
void ui_dev_channel_destroy(ui_dev_channel *ch);
// Called after each post, from the posting thread (ui_app_request_turn).
void ui_dev_channel_set_wake(ui_dev_channel *ch, void (*wake)(void *user), void *user);
// Takes ownership of the batch (malloc'd, with its files). Thread: any.
void ui_dev_channel_post(ui_dev_channel *ch, ui_dev_batch *batch);
void ui_dev_batch_free(ui_dev_batch *batch);

// The host's side. Thread: logic.
typedef struct ui_dev ui_dev;

ui_result ui_dev_create(ui_dev_channel *ch, ui_js *js, ui_dev **out);
void ui_dev_destroy(ui_dev *d);
char *ui_dev_resolve(void *dev, const char *base, const char *specifier);
bool ui_dev_load(void *dev, const char *name, ui_js_module_source *out);
// The host API's dev functions, added to `axe:host`'s exports.
bool ui_dev_install(ui_dev *d, ui_js_val exports);
// Moves what was posted into the table, and returns true when the hot runtime should see it.
bool ui_dev_take(ui_dev *d);
// Calls the hot runtime's handler with what ui_dev_take took: the first batch included, which
// is how the entry gets evaluated, so build errors at startup reach the overlay like any other.
// False: an exception is pending.
bool ui_dev_deliver(ui_dev *d);
// The newest applied batch's event time, or 0, once: for the hot update trace.
uint64_t ui_dev_take_event_ns(ui_dev *d);
// JS references, released before the engine goes.
void ui_dev_release(ui_dev *d);
