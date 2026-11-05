// ui_watch on inotify. A file's events are collected until the directory has been quiet for the
// debounce, at most 10 times that from the first event, then reported once.
#include <dirent.h>
#include <stdio.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#include "build.h"
#include "runtime/common.h"
#include "watch.h"

#define MASK (IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM | IN_CREATE | IN_DELETE | IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR)

typedef struct dir_watch {
    int wd;
    char *path; // relative to the root; "" for the root
} dir_watch;

struct ui_watch {
    char *root;
    int fd, wake;
    dir_watch *dirs;
    uint32_t dir_count;
    char **known; // every source seen: a directory moved away reports nothing per file
    uint32_t known_count;
    char **pending;
    uint32_t pending_count;
};

static char *join(const char *a, const char *b) {
    size_t n = strlen(a) + strlen(b) + 2;
    char *s = malloc(n);
    if (s) {
        if (a[0]) snprintf(s, n, "%s/%s", a, b);
        else snprintf(s, n, "%s", b);
    }
    return s;
}

static bool has(char **list, uint32_t count, const char *s) {
    for (uint32_t i = 0; i < count; i++)
        if (!strcmp(list[i], s)) return true;
    return false;
}

static void add(char ***list, uint32_t *count, const char *s) {
    if (has(*list, *count, s)) return;
    char **grown = realloc(*list, (*count + 1) * sizeof *grown);
    char *copy = strdup(s);
    if (!grown || !copy) {
        if (grown) *list = grown;
        free(copy);
        return;
    }
    *list = grown;
    (*list)[(*count)++] = copy;
}

static void source_changed(ui_watch *w, const char *path) {
    add(&w->pending, &w->pending_count, path);
    add(&w->known, &w->known_count, path);
}

// Watches `rel` and what is below it. Files already there count as changed when `report`: a
// directory that just appeared may have been filled before its watch existed.
static void watch_tree(ui_watch *w, const char *rel, bool report) {
    char *full = rel[0] ? join(w->root, rel) : strdup(w->root);
    if (!full) return;
    int wd = inotify_add_watch(w->fd, full, MASK);
    if (wd >= 0) {
        bool seen = false;
        for (uint32_t i = 0; i < w->dir_count; i++) seen = seen || w->dirs[i].wd == wd;
        dir_watch *grown = seen ? NULL : realloc(w->dirs, (w->dir_count + 1) * sizeof *grown);
        if (grown) {
            w->dirs = grown;
            w->dirs[w->dir_count++] = (dir_watch){wd, strdup(rel)};
        }
    }
    DIR *d = opendir(full);
    for (struct dirent *e; d && (e = readdir(d));) {
        if (e->d_name[0] == '.' || !strcmp(e->d_name, "node_modules")) continue;
        char *child = join(rel, e->d_name), *child_full = child ? join(w->root, child) : NULL;
        struct stat st;
        if (child_full && stat(child_full, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                watch_tree(w, child, report);
            } else if (ui_dev_is_source(child)) {
                add(&w->known, &w->known_count, child);
                if (report) add(&w->pending, &w->pending_count, child);
            }
        }
        free(child);
        free(child_full);
    }
    if (d) closedir(d);
    free(full);
}

static const char *dir_of(ui_watch *w, int wd) {
    for (uint32_t i = 0; i < w->dir_count; i++)
        if (w->dirs[i].wd == wd) return w->dirs[i].path;
    return NULL;
}

static void forget_dir(ui_watch *w, int wd) {
    for (uint32_t i = 0; i < w->dir_count; i++) {
        if (w->dirs[i].wd != wd) continue;
        free(w->dirs[i].path);
        w->dirs[i] = w->dirs[--w->dir_count];
        return;
    }
}

// A directory went away: what was known under it is gone, and its watches with it.
static void dir_gone(ui_watch *w, const char *rel) {
    size_t n = strlen(rel);
    for (uint32_t i = 0; i < w->known_count; i++)
        if (!strncmp(w->known[i], rel, n) && w->known[i][n] == '/') add(&w->pending, &w->pending_count, w->known[i]);
    for (uint32_t i = w->dir_count; i-- > 0;) {
        const char *p = w->dirs[i].path;
        if (strncmp(p, rel, n) || (p[n] != '/' && p[n] != 0)) continue;
        inotify_rm_watch(w->fd, w->dirs[i].wd);
        forget_dir(w, w->dirs[i].wd);
    }
}

static void handle(ui_watch *w, const struct inotify_event *e) {
    if (e->mask & (IN_IGNORED | IN_Q_OVERFLOW)) {
        if (e->mask & IN_IGNORED) forget_dir(w, e->wd);
        else watch_tree(w, "", true); // events were lost: everything counts as changed
        return;
    }
    if (e->mask & (IN_DELETE_SELF | IN_MOVE_SELF) || !e->len) return; // the parent reports it
    const char *dir = dir_of(w, e->wd);
    if (!dir) return;
    char *rel = join(dir, e->name);
    if (!rel) return;
    if (e->mask & IN_ISDIR) {
        if (e->mask & (IN_CREATE | IN_MOVED_TO)) watch_tree(w, rel, true);
        else if (e->mask & (IN_DELETE | IN_MOVED_FROM)) dir_gone(w, rel);
    } else if (ui_dev_is_source(rel)) {
        source_changed(w, rel);
    }
    free(rel);
}

// Reads what is queued. False: interrupted.
static bool drain(ui_watch *w) {
    _Alignas(struct inotify_event) char buffer[16 * 1024];
    for (;;) {
        ssize_t n = read(w->fd, buffer, sizeof buffer);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return true; // non-blocking: nothing more
        for (char *p = buffer; p < buffer + n;) {
            const struct inotify_event *e = (const struct inotify_event *)p;
            handle(w, e);
            p += sizeof *e + e->len;
        }
    }
}

ui_result ui_watch_open(const char *root, ui_watch **out) {
    ui_watch *w = calloc(1, sizeof *w);
    if (!w) return UI_ERR_OUT_OF_MEMORY;
    w->root = strdup(root);
    w->fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    w->wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (!w->root || w->fd < 0 || w->wake < 0) {
        ui_watch_close(w);
        return UI_ERR_PLATFORM;
    }
    watch_tree(w, "", false);
    if (!w->dir_count) {
        ui_watch_close(w);
        return UI_ERR_IO;
    }
    *out = w;
    return UI_OK;
}

void ui_watch_interrupt(ui_watch *w) {
    uint64_t one = 1;
    ssize_t r = write(w->wake, &one, sizeof one);
    (void)r;
}

static int compare_paths(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

bool ui_watch_next(ui_watch *w, int timeout_ms, ui_watch_batch *out) {
    *out = (ui_watch_batch){0};
    struct pollfd fds[2] = {{.fd = w->fd, .events = POLLIN}, {.fd = w->wake, .events = POLLIN}};
    uint64_t first = 0, last = 0;
    for (;;) {
        int wait = timeout_ms;
        if (w->pending_count) {
            uint64_t now = ui_monotonic_ns(), quiet = last + UI_WATCH_DEBOUNCE_MS * 1000000ull;
            uint64_t limit = first + UI_WATCH_DEBOUNCE_MS * 10 * 1000000ull;
            uint64_t until = quiet < limit ? quiet : limit;
            if (now >= until) break;
            wait = (int)((until - now) / 1000000u) + 1;
        }
        int r = poll(fds, 2, wait);
        if (r < 0 && errno == EINTR) continue;
        if (fds[1].revents & POLLIN) {
            uint64_t v;
            ssize_t n = read(w->wake, &v, sizeof v);
            (void)n;
            return false;
        }
        if (r == 0 && !w->pending_count) return false;
        if (r > 0 && (fds[0].revents & POLLIN)) {
            uint64_t before = w->pending_count;
            uint64_t now = ui_monotonic_ns();
            drain(w);
            if (w->pending_count && !first) first = now;
            if (w->pending_count != before || r > 0) last = now;
        }
    }
    qsort(w->pending, w->pending_count, sizeof *w->pending, compare_paths);
    out->paths = w->pending;
    out->count = w->pending_count;
    out->event_ns = first;
    w->pending = NULL;
    w->pending_count = 0;
    return true;
}

void ui_watch_batch_free(ui_watch_batch *b) {
    for (uint32_t i = 0; i < b->count; i++) free(b->paths[i]);
    free(b->paths);
    *b = (ui_watch_batch){0};
}

void ui_watch_close(ui_watch *w) {
    if (!w) return;
    if (w->fd >= 0) close(w->fd);
    if (w->wake >= 0) close(w->wake);
    for (uint32_t i = 0; i < w->dir_count; i++) free(w->dirs[i].path);
    for (uint32_t i = 0; i < w->known_count; i++) free(w->known[i]);
    for (uint32_t i = 0; i < w->pending_count; i++) free(w->pending[i]);
    free(w->dirs);
    free(w->known);
    free(w->pending);
    free(w->root);
    free(w);
}
