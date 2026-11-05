// See session.h.
#include "session.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <threads.h>

#include "runtime/common.h"
#include "watch.h"

struct ui_dev_session {
    ui_dev_build_desc build;
    ui_dev_channel *channel;
    ui_watch *watch;
    thrd_t thread;
    atomic_bool stopping;
};

static int session_main(void *arg) {
    ui_dev_session *s = arg;
    uint32_t count = 0;
    char **sources = ui_dev_list_sources(s->build.root, &count);
    ui_dev_batch *first = ui_dev_build(&s->build, (const char *const *)sources, count, 0);
    if (first) ui_dev_channel_post(s->channel, first);
    for (uint32_t i = 0; i < count; i++) free(sources[i]);
    free(sources);
    while (!atomic_load(&s->stopping)) {
        ui_watch_batch b;
        if (!ui_watch_next(s->watch, -1, &b)) continue;
        ui_dev_batch *update = ui_dev_build(&s->build, (const char *const *)b.paths, b.count, b.event_ns);
        if (update) ui_dev_channel_post(s->channel, update);
        ui_watch_batch_free(&b);
    }
    return 0;
}

ui_dev_session *ui_dev_session_start(const ui_dev_build_desc *desc, ui_dev_channel *channel) {
    ui_dev_session *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->build = *desc;
    s->channel = channel;
    // The watch comes first: an edit during the first build isn't lost.
    if (ui_watch_open(desc->root, &s->watch) != UI_OK || thrd_create(&s->thread, session_main, s) != thrd_success) {
        ui_watch_close(s->watch);
        free(s);
        return NULL;
    }
    return s;
}

void ui_dev_session_stop(ui_dev_session *s) {
    if (!s) return;
    atomic_store(&s->stopping, true);
    ui_watch_interrupt(s->watch);
    thrd_join(s->thread, NULL);
    ui_watch_close(s->watch);
    free(s);
}
