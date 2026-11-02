// Internal helpers shared by every module: logging, debug assertions, small utilities.
#pragma once

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "ui/result.h"

typedef enum ui_log_level {
    UI_LOG_ERROR = 0,
    UI_LOG_WARN,
    UI_LOG_INFO,
    UI_LOG_DEBUG,
    UI_LOG_TRACE,
} ui_log_level;

// Per-app logger. Immutable after app creation, so any thread may use it.
typedef struct ui_logger {
    ui_log_level level;
} ui_logger;

// Reads UI_LOG=error|warn|info|debug|trace (default info).
void ui_logger_init_from_env(ui_logger *log);

[[gnu::format(printf, 4, 5)]]
void ui_log_write(const ui_logger *log, ui_log_level level, const char *module, const char *fmt,
                  ...);

#define UI_LOG(log, lvl, module, ...)                                                          \
    do {                                                                                       \
        if ((log)->level >= (lvl)) ui_log_write((log), (lvl), (module), __VA_ARGS__);         \
    } while (0)
#define UI_ERROR(log, module, ...) UI_LOG(log, UI_LOG_ERROR, module, __VA_ARGS__)
#define UI_WARN(log, module, ...) UI_LOG(log, UI_LOG_WARN, module, __VA_ARGS__)
#define UI_INFO(log, module, ...) UI_LOG(log, UI_LOG_INFO, module, __VA_ARGS__)
#define UI_DEBUG(log, module, ...) UI_LOG(log, UI_LOG_DEBUG, module, __VA_ARGS__)
#define UI_TRACE_LOG(log, module, ...) UI_LOG(log, UI_LOG_TRACE, module, __VA_ARGS__)

// Debug-only diagnostics (stale handles, dropped commands). Compiled out in release.
#ifdef NDEBUG
#define UI_DEBUG_REPORT(log, module, ...) ((void)0)
#else
#define UI_DEBUG_REPORT(log, module, ...) UI_WARN(log, module, __VA_ARGS__)
#endif

// Debug assertions are the only place library code may abort.
#ifdef NDEBUG
#define UI_ASSERT(cond) ((void)0)
#else
[[noreturn]] void ui_assert_fail(const char *expr, const char *file, int line);
#define UI_ASSERT(cond) ((cond) ? (void)0 : ui_assert_fail(#cond, __FILE__, __LINE__))
#endif

#define UI_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

// Grows *items (element size `size`) to hold at least `needed`; doubles capacity.
ui_result ui_grow(void **items, uint32_t *capacity, uint32_t needed, size_t size);

// CLOCK_MONOTONIC in nanoseconds.
uint64_t ui_monotonic_ns(void);

// Reads an environment flag: unset, empty or "0" is false.
bool ui_env_flag(const char *name);
