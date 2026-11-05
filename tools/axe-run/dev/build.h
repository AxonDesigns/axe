// Rebuilds an app's changed files for hot reload (docs/ui.md, "Hot reload"): axe-tsx --dev, then
// esbuild per file, as subprocesses. Used by axe-run --dev and the test runner.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "script/host/dev.h"

typedef struct ui_dev_build_desc {
    const char *root;    // the app's sources
    const char *work;    // a directory for the build's output (created)
    const char *axe_tsx; // the tools' paths
    const char *esbuild;
    const char *source_dir; // AXE_SOURCE_DIR in the app
} ui_dev_build_desc;

// One batch for the files (relative to the root): each is its code, the build's error, or
// deleted if the file is gone. Ownership: post it to a channel, or ui_dev_batch_free it.
ui_dev_batch *ui_dev_build(const ui_dev_build_desc *desc, const char *const *files, uint32_t count, uint64_t event_ns);
// The app's sources under the root: *.ts and *.tsx, relative to it, without *.check.* type tests.
// Ownership: free each and the list.
char **ui_dev_list_sources(const char *root, uint32_t *count);
// Whether the watcher should care about a path (relative to the root).
bool ui_dev_is_source(const char *path);
