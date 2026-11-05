// See build.h.
#include "build.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

// Runs argv, with the child's stderr in *err (malloc'd, NUL-terminated). Returns its exit status,
// or -1 if it didn't run.
static int run(const char *const *argv, char **err) {
    int fds[2];
    *err = NULL;
    if (pipe2(fds, O_CLOEXEC) != 0) return -1;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDERR_FILENO);
    pid_t pid;
    int r = posix_spawn(&pid, argv[0], &actions, NULL, (char *const *)(uintptr_t)argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(fds[1]);
    if (r != 0) {
        close(fds[0]);
        return -1;
    }
    size_t len = 0, capacity = 4096;
    char *text = malloc(capacity);
    while (text) {
        if (len + 1 == capacity) {
            char *grown = realloc(text, capacity *= 2);
            if (!grown) break;
            text = grown;
        }
        ssize_t n = read(fds[0], text + len, capacity - len - 1);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        len += (size_t)n;
    }
    close(fds[0]);
    if (text) text[len] = 0;
    *err = text;
    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static char *join(const char *a, const char *b) {
    size_t n = strlen(a) + strlen(b) + 2;
    char *s = malloc(n);
    if (s) snprintf(s, n, "%s/%s", a, b);
    return s;
}

static char *read_all(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *data = NULL;
    if (fseek(f, 0, SEEK_END) == 0) {
        long n = ftell(f);
        if (n >= 0 && fseek(f, 0, SEEK_SET) == 0 && (data = malloc((size_t)n + 1))) {
            if (fread(data, 1, (size_t)n, f) == (size_t)n) {
                data[n] = 0;
                *size = (size_t)n;
            } else {
                free(data);
                data = NULL;
            }
        }
    }
    fclose(f);
    return data;
}

// The output's path: esbuild names it .js whatever it was.
static char *js_path(const char *dir, const char *file) {
    char *path = join(dir, file);
    char *dot = path ? strrchr(path, '.') : NULL;
    if (dot) strcpy(dot, ".js");
    return path;
}

// The lines of `text` about `file`, or NULL if there are none. Other errors have no file.
static char *errors_of(const char *text, const char *file) {
    size_t n = strlen(file), len = 0;
    char *out = malloc(strlen(text) + 1);
    if (!out) return NULL;
    for (const char *line = text; *line;) {
        const char *end = strchr(line, '\n');
        size_t size = end ? (size_t)(end - line) + 1 : strlen(line);
        if (!strncmp(line, file, n) && line[n] == ':' && strstr(line, ": error: ") && strstr(line, ": error: ") < line + size) {
            memcpy(out + len, line, size);
            len += size;
        }
        line += size;
    }
    while (len && out[len - 1] == '\n') len--;
    out[len] = 0;
    if (!len) {
        free(out);
        return NULL;
    }
    return out;
}

ui_dev_batch *ui_dev_build(const ui_dev_build_desc *d, const char *const *files, uint32_t count, uint64_t event_ns) {
    ui_dev_batch *b = calloc(1, sizeof *b);
    if (b) b->files = calloc(count ? count : 1, sizeof *b->files);
    if (!b || !b->files) {
        free(b);
        return NULL;
    }
    b->count = count;
    b->event_ns = event_ns;
    char *tsx_dir = join(d->work, "tsx"), *js_dir = join(d->work, "js");

    // What exists goes through axe-tsx; what doesn't is reported deleted.
    const char **tsx_argv = calloc((size_t)count + 7, sizeof *tsx_argv);
    char **sources = calloc(count ? count : 1, sizeof *sources);
    uint32_t tsx_argc = 0, present = 0;
    tsx_argv[tsx_argc++] = d->axe_tsx;
    tsx_argv[tsx_argc++] = "--dev";
    tsx_argv[tsx_argc++] = "--root";
    tsx_argv[tsx_argc++] = d->root;
    tsx_argv[tsx_argc++] = "--out";
    tsx_argv[tsx_argc++] = tsx_dir;
    for (uint32_t i = 0; i < count; i++) {
        b->files[i].path = strdup(files[i]);
        char *path = join(d->root, files[i]);
        struct stat st;
        if (stat(path, &st) != 0) {
            b->files[i].deleted = true;
            free(path);
            continue;
        }
        sources[i] = path;
        tsx_argv[tsx_argc++] = path;
        present++;
    }
    char *err = NULL;
    bool tsx_ok = true;
    if (present) {
        int status = run(tsx_argv, &err);
        tsx_ok = status == 0;
        if (err && !tsx_ok) {
            for (uint32_t i = 0; i < count; i++)
                if (sources[i]) b->files[i].error = errors_of(err, files[i]);
        }
        // Warnings are for the developer's terminal.
        if (err && err[0]) fputs(err, stderr);
        if (status < 0) {
            for (uint32_t i = 0; i < count; i++) {
                if (!sources[i]) continue;
                free(b->files[i].error);
                b->files[i].error = strdup("axe-tsx didn't run");
            }
        }
    }
    bool unexplained = !tsx_ok;
    for (uint32_t i = 0; i < count; i++)
        if (b->files[i].error) unexplained = false;
    if (unexplained) {
        for (uint32_t i = 0; i < count; i++)
            if (sources[i]) b->files[i].error = strdup(err && err[0] ? err : "axe-tsx failed");
    }
    free(err);

    // esbuild once over what compiled, per file (no bundling).
    const char **es_argv = calloc((size_t)count + 12, sizeof *es_argv);
    char **inputs = calloc(count ? count : 1, sizeof *inputs);
    char define[PATH_MAX + 32], outdir[PATH_MAX + 16], outbase[PATH_MAX + 16];
    snprintf(define, sizeof define, "--define:AXE_SOURCE_DIR=\"%s\"", d->source_dir);
    snprintf(outdir, sizeof outdir, "--outdir=%s", js_dir);
    snprintf(outbase, sizeof outbase, "--outbase=%s", tsx_dir);
    uint32_t es_argc = 0, inputs_count = 0;
    es_argv[es_argc++] = d->esbuild;
    for (uint32_t i = 0; i < count; i++) {
        if (!sources[i] || b->files[i].error) continue;
        inputs[i] = join(tsx_dir, files[i]);
        es_argv[es_argc++] = inputs[i];
        inputs_count++;
    }
    es_argv[es_argc++] = "--format=esm";
    es_argv[es_argc++] = "--loader:.tsx=ts";
    es_argv[es_argc++] = "--target=es2023";
    es_argv[es_argc++] = "--sourcemap=inline";
    es_argv[es_argc++] = "--keep-names";
    es_argv[es_argc++] = "--log-level=error";
    es_argv[es_argc++] = define;
    es_argv[es_argc++] = outdir;
    es_argv[es_argc++] = outbase;
    if (inputs_count) {
        int status = run(es_argv, &err);
        for (uint32_t i = 0; i < count; i++) {
            if (!inputs[i]) continue;
            if (status != 0) {
                b->files[i].error = strdup(err && err[0] ? err : "esbuild failed");
                continue;
            }
            char *out = js_path(js_dir, files[i]);
            b->files[i].code = read_all(out, &b->files[i].size);
            if (!b->files[i].code) b->files[i].error = strdup("esbuild wrote no output");
            free(out);
        }
        free(err);
    }
    for (uint32_t i = 0; i < count; i++) {
        free(sources[i]);
        free(inputs[i]);
    }
    free(sources);
    free(inputs);
    free(tsx_argv);
    free(es_argv);
    free(tsx_dir);
    free(js_dir);
    return b;
}

bool ui_dev_is_source(const char *path) {
    size_t n = strlen(path);
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (base[0] == '.') return false;
    bool ts = n > 3 && !strcmp(path + n - 3, ".ts");
    bool tsx = n > 4 && !strcmp(path + n - 4, ".tsx");
    return (ts || tsx) && !strstr(base, ".check.") && !strstr(base, ".d.ts");
}

static void walk(const char *root, const char *rel, char ***list, uint32_t *count) {
    char *dir = rel[0] ? join(root, rel) : strdup(root);
    DIR *d = dir ? opendir(dir) : NULL;
    free(dir);
    if (!d) return;
    for (struct dirent *e; (e = readdir(d));) {
        if (e->d_name[0] == '.' || !strcmp(e->d_name, "node_modules")) continue;
        char *path = rel[0] ? join(rel, e->d_name) : strdup(e->d_name);
        char *full = path ? join(root, path) : NULL;
        struct stat st;
        if (full && stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
            walk(root, path, list, count);
        } else if (path && ui_dev_is_source(path)) {
            char **grown = realloc(*list, (*count + 1) * sizeof *grown);
            if (grown) {
                *list = grown;
                (*list)[(*count)++] = path;
                path = NULL;
            }
        }
        free(path);
        free(full);
    }
    closedir(d);
}

char **ui_dev_list_sources(const char *root, uint32_t *count) {
    char **list = NULL;
    *count = 0;
    walk(root, "", &list, count);
    return list;
}
