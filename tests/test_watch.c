// The watcher (tools/axe-run/dev/watch.h): how editors really save, and bursts.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <threads.h>
#include <unistd.h>

#include "test.h"
#include "watch.h"

static char root[4096];

static void path_of(char *out, size_t size, const char *rel) { snprintf(out, size, "%s/%s", root, rel); }

static void put(const char *rel, const char *text) {
    char p[4200];
    path_of(p, sizeof p, rel);
    FILE *f = fopen(p, "wb");
    CHECK(f != NULL);
    fputs(text, f);
    fclose(f);
}

static void sleep_ms(int ms) { thrd_sleep(&(struct timespec){.tv_nsec = ms * 1000000L}, NULL); }

static ui_watch *fresh(void) {
    char command[4300];
    snprintf(command, sizeof command, "rm -rf '%s' && mkdir -p '%s'", root, root);
    CHECK(system(command) == 0);
    put("a.tsx", "1");
    ui_watch *w;
    CHECK(ui_watch_open(root, &w) == UI_OK);
    return w;
}

// The next batch must be exactly `want` (comma separated, sorted).
static void expect_batch(ui_watch *w, const char *want) {
    ui_watch_batch b;
    bool got = ui_watch_next(w, 2000, &b);
    CHECK(got);
    if (!got) return;
    char list[1024] = "";
    for (uint32_t i = 0; i < b.count; i++) {
        if (i) strcat(list, ",");
        strcat(list, b.paths[i]);
    }
    if (strcmp(list, want)) fprintf(stderr, "  got \"%s\", want \"%s\"\n", list, want);
    CHECK(!strcmp(list, want));
    CHECK(b.event_ns != 0);
    ui_watch_batch_free(&b);
}

static void expect_quiet(ui_watch *w) {
    ui_watch_batch b;
    bool got = ui_watch_next(w, 60, &b);
    if (got) {
        fprintf(stderr, "  unexpected batch starting %s\n", b.paths[0]);
        ui_watch_batch_free(&b);
    }
    CHECK(!got);
}

static void test_write_in_place(void) {
    ui_watch *w = fresh();
    put("a.tsx", "2");
    expect_batch(w, "a.tsx");
    expect_quiet(w);
    ui_watch_close(w);
}

static void test_atomic_save(void) {
    ui_watch *w = fresh();
    put("a.tsx.tmp~", "2");
    char from[4200], to[4200];
    path_of(from, sizeof from, "a.tsx.tmp~");
    path_of(to, sizeof to, "a.tsx");
    CHECK(rename(from, to) == 0);
    expect_batch(w, "a.tsx"); // and not the temporary file
    expect_quiet(w);
    ui_watch_close(w);
}

static void test_delete_then_recreate(void) {
    ui_watch *w = fresh();
    char p[4200];
    path_of(p, sizeof p, "a.tsx");
    CHECK(unlink(p) == 0);
    put("a.tsx", "2");
    expect_batch(w, "a.tsx"); // one change
    expect_quiet(w);
    // And with a pause longer than the debounce: two, the caller sees gone, then there.
    CHECK(unlink(p) == 0);
    expect_batch(w, "a.tsx");
    put("a.tsx", "3");
    expect_batch(w, "a.tsx");
    ui_watch_close(w);
}

static void test_vim_backup(void) {
    // Vim's default: the file moves to a backup, a new one takes its name.
    ui_watch *w = fresh();
    char from[4200], to[4200];
    path_of(from, sizeof from, "a.tsx");
    path_of(to, sizeof to, "a.tsx~");
    CHECK(rename(from, to) == 0);
    put("4913", "");
    put("a.tsx", "2");
    CHECK(unlink(to) == 0);
    expect_batch(w, "a.tsx");
    expect_quiet(w);
    ui_watch_close(w);
}

static void test_burst(void) {
    ui_watch *w = fresh();
    put("b.ts", "1");
    for (int i = 0; i < 20; i++) {
        put("a.tsx", "x");
        sleep_ms(2);
    }
    put("b.ts", "2");
    expect_batch(w, "a.tsx,b.ts");
    expect_quiet(w);
    ui_watch_close(w);
}

static void test_ignored(void) {
    ui_watch *w = fresh();
    put(".hidden.ts", "1");
    put("notes.md", "1");
    put("types.check.ts", "1");
    put(".a.tsx.swp", "1");
    expect_quiet(w);
    ui_watch_close(w);
}

static void test_create_and_delete(void) {
    ui_watch *w = fresh();
    put("new.ts", "1");
    expect_batch(w, "new.ts");
    char p[4200];
    path_of(p, sizeof p, "new.ts");
    CHECK(unlink(p) == 0);
    expect_batch(w, "new.ts");
    ui_watch_close(w);
}

static void test_rename_file(void) {
    ui_watch *w = fresh();
    char from[4200], to[4200];
    path_of(from, sizeof from, "a.tsx");
    path_of(to, sizeof to, "b.tsx");
    CHECK(rename(from, to) == 0);
    expect_batch(w, "a.tsx,b.tsx"); // one gone, one new
    ui_watch_close(w);
}

static void test_directories(void) {
    ui_watch *w = fresh();
    char dir[4200];
    path_of(dir, sizeof dir, "sub");
    CHECK(mkdir(dir, 0777) == 0);
    put("sub/x.tsx", "1"); // written before the watch can exist
    expect_batch(w, "sub/x.tsx");
    put("sub/x.tsx", "2");
    expect_batch(w, "sub/x.tsx");
    // A directory moved away reports its files.
    char moved[4300];
    snprintf(moved, sizeof moved, "%s-moved", root);
    char command[8700];
    snprintf(command, sizeof command, "rm -rf '%s'", moved);
    CHECK(system(command) == 0);
    CHECK(rename(dir, moved) == 0);
    expect_batch(w, "sub/x.tsx");
    CHECK(system(command) == 0);
    ui_watch_close(w);
}

static int interrupter(void *w) {
    sleep_ms(20);
    ui_watch_interrupt(w);
    return 0;
}

static void test_interrupt(void) {
    ui_watch *w = fresh();
    thrd_t t;
    CHECK(thrd_create(&t, interrupter, w) == thrd_success);
    ui_watch_batch b;
    CHECK(!ui_watch_next(w, -1, &b));
    thrd_join(t, NULL);
    ui_watch_close(w);
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    snprintf(root, sizeof root, "%s", argv[1]);
    RUN(test_write_in_place);
    RUN(test_atomic_save);
    RUN(test_delete_then_recreate);
    RUN(test_vim_backup);
    RUN(test_burst);
    RUN(test_ignored);
    RUN(test_create_and_delete);
    RUN(test_rename_file);
    RUN(test_directories);
    RUN(test_interrupt);
    return test_failures ? 1 : 0;
}
