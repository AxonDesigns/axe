#include "runtime/common.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *const level_names[] = {"error", "warn", "info", "debug", "trace"};

void ui_logger_init_from_env(ui_logger *log) {
    log->level = UI_LOG_INFO;
    const char *env = getenv("UI_LOG");
    if (!env) return;
    for (size_t i = 0; i < UI_ARRAY_LEN(level_names); i++) {
        if (strcmp(env, level_names[i]) == 0) log->level = (ui_log_level)i;
    }
}

void ui_log_write(const ui_logger *log, ui_log_level level, const char *module, const char *fmt,
                  ...) {
    (void)log;
    char line[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof line, fmt, args);
    va_end(args);
    // One fprintf per line keeps lines from different threads whole.
    fprintf(stderr, "[%s] %s: %s\n", level_names[level], module, line);
}

#ifndef NDEBUG
void ui_assert_fail(const char *expr, const char *file, int line) {
    fprintf(stderr, "assertion failed: %s (%s:%d)\n", expr, file, line);
    abort();
}
#endif

ui_result ui_grow(void **items, uint32_t *capacity, uint32_t needed, size_t size) {
    if (needed <= *capacity) return UI_OK;
    uint32_t cap = *capacity ? *capacity : 8;
    while (cap < needed) {
        if (cap > UINT32_MAX / 2) return UI_ERR_OUT_OF_MEMORY;
        cap *= 2;
    }
    void *grown = realloc(*items, (size_t)cap * size);
    if (!grown) return UI_ERR_OUT_OF_MEMORY;
    *items = grown;
    *capacity = cap;
    return UI_OK;
}

uint64_t ui_monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

bool ui_env_flag(const char *name) {
    const char *v = getenv(name);
    return v && v[0] && strcmp(v, "0") != 0;
}

const char *ui_result_str(ui_result result) {
    switch (result) {
    case UI_OK: return "ok";
    case UI_ERR_INVALID_ARGUMENT: return "invalid argument";
    case UI_ERR_OUT_OF_MEMORY: return "out of memory";
    case UI_ERR_UNSUPPORTED: return "unsupported";
    case UI_ERR_PLATFORM: return "platform error";
    case UI_ERR_GPU: return "gpu error";
    case UI_ERR_STALE_HANDLE: return "stale handle";
    case UI_ERR_MALFORMED: return "malformed buffer";
    case UI_ERR_FULL: return "full";
    case UI_ERR_IO: return "i/o error";
    case UI_ERR_END: return "end";
    case UI_ERR_WRONG_STATE: return "wrong state";
    }
    return "unknown";
}
