// The generated codec reads every fixture buffer as the hand-written code did before the IDL
// (tests/wire/proof.md): each tests/wire/fixtures/<name>.bin is dumped and compared with the
// <name>.bin.txt the pre-IDL code produced. cmd-* are command buffers, ev-* event buffers.
#include <dirent.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"
#include "wire_dump.h"

static char *slurp(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *data = malloc((size_t)n + 1);
    if (data && fread(data, 1, (size_t)n, f) != (size_t)n) {
        free(data);
        data = NULL;
    }
    fclose(f);
    if (data) data[n] = 0;
    *size = (size_t)n;
    return data;
}

static void test_fixtures(void) {
    DIR *d = opendir(UI_WIRE_FIXTURES);
    CHECK(d != NULL);
    if (!d) return;
    unsigned commands = 0, events = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t len = strlen(e->d_name);
        if (len < 4 || strcmp(e->d_name + len - 4, ".bin")) continue;
        bool is_events = !strncmp(e->d_name, "ev-", 3);
        char path[4096], expected_path[4096];
        snprintf(path, sizeof path, "%s/%s", UI_WIRE_FIXTURES, e->d_name);
        snprintf(expected_path, sizeof expected_path, "%s.txt", path);
        size_t size, expected_size;
        char *data = slurp(path, &size), *expected = slurp(expected_path, &expected_size);
        CHECK(data && expected);
        if (data && expected) {
            wire_text t = {0};
            (is_events ? wire_dump_events : wire_dump_commands)(&t, (const uint8_t *)data, size);
            if (t.size != expected_size || memcmp(t.data, expected, t.size)) {
                fprintf(stderr, "%s decodes differently from its dump\n--- expected\n%s--- got\n%s", e->d_name,
                        expected, t.data);
                test_failures++;
            }
            wire_text_free(&t);
            if (is_events) events++;
            else commands++;
        }
        free(data);
        free(expected);
    }
    closedir(d);
    printf("%u command and %u event fixtures\n", commands, events);
    CHECK(commands == 395 && events == 180); // the fixtures are all there
}

int main(void) {
    RUN(test_fixtures);
    return test_failures ? 1 : 0;
}
