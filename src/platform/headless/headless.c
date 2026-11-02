// Headless platform: surfaces with no window. Frames are produced only on request
// (ui_app_frame), so there is nothing to pace; wait() just sleeps on the wake fd.
#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "platform/platform.h"

typedef struct headless_platform {
    ui_platform base;
    int wake_fd;
} headless_platform;

struct ui_psurface {
    void *core_tag;
};

static void destroy(ui_platform *p) {
    headless_platform *h = (headless_platform *)p;
    close(h->wake_fd);
    free(h);
}

static ui_result surface_create(ui_platform *p, const ui_surface_desc *desc, void *core_tag,
                                ui_psurface **out) {
    if (desc->kind != UI_SURFACE_HEADLESS || !desc->width || !desc->height)
        return UI_ERR_INVALID_ARGUMENT;
    ui_psurface *s = malloc(sizeof *s);
    if (!s) return UI_ERR_OUT_OF_MEMORY;
    s->core_tag = core_tag;
    *out = s;
    ui_surface_config config = {
        .logical_width = desc->width,
        .logical_height = desc->height,
        .scale_120 = desc->scale_120 ? desc->scale_120 : 120,
        .token = 0,
    };
    p->listener.configure(p->listener.user, core_tag, &config);
    return UI_OK;
}

static void surface_destroy(ui_platform *p, ui_psurface *s) {
    (void)p;
    free(s);
}

static void surface_native(ui_psurface *s, ui_native_surface *out) {
    (void)s;
    *out = (ui_native_surface){.kind = UI_NATIVE_OFFSCREEN};
}

static void surface_pre_present(ui_psurface *s, const ui_present_info *info) {
    (void)s;
    (void)info;
}

static ui_result surface_set_position(ui_psurface *s, int32_t x, int32_t y) {
    (void)s;
    (void)x;
    (void)y;
    return UI_ERR_UNSUPPORTED;
}

static ui_result wait_events(ui_platform *p, uint64_t timeout_ns) {
    headless_platform *h = (headless_platform *)p;
    struct pollfd fd = {.fd = h->wake_fd, .events = POLLIN};
    int ms = timeout_ns == UINT64_MAX ? -1 : (int)(timeout_ns / 1000000u);
    if (poll(&fd, 1, ms) < 0 && errno != EINTR) return UI_ERR_PLATFORM;
    uint64_t drain;
    if (fd.revents & POLLIN) (void)!read(h->wake_fd, &drain, sizeof drain);
    return UI_OK;
}

static void wake(ui_platform *p) {
    headless_platform *h = (headless_platform *)p;
    uint64_t one = 1;
    (void)!write(h->wake_fd, &one, sizeof one);
}

static const ui_platform_vtable vtable = {
    .destroy = destroy,
    .surface_create = surface_create,
    .surface_destroy = surface_destroy,
    .surface_native = surface_native,
    .surface_pre_present = surface_pre_present,
    .surface_set_position = surface_set_position,
    .wait = wait_events,
    .wake = wake,
};

ui_result ui_platform_headless_create(const ui_logger *log, ui_platform_listener listener,
                                      ui_platform **out) {
    headless_platform *h = calloc(1, sizeof *h);
    if (!h) return UI_ERR_OUT_OF_MEMORY;
    h->wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (h->wake_fd < 0) {
        free(h);
        return UI_ERR_PLATFORM;
    }
    h->base = (ui_platform){.vt = &vtable, .listener = listener, .log = log};
    *out = &h->base;
    return UI_OK;
}
