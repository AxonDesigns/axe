// The render actor: the sole owner of the GPU. Consumes display lists from surface
// mailboxes and presents them. This header is WebGPU-free; webgpu.h stays inside src/render.
#pragma once

#include <stdatomic.h>

#include "core/display_list.h"
#include "hooks/hooks_internal.h"
#include "platform/platform.h"
#include "runtime/actor.h"
#include "ui/app.h"

// Payload of UI_MSG_SURFACE_ADD (malloc'd; the render actor frees it).
typedef struct ui_render_surface_desc {
    ui_handle surface;
    ui_native_surface native;
    ui_mailbox *mailbox;     // owned by the core surface; outlives the render actor
    ui_platform *platform;   // for surface_pre_present
    ui_psurface *psurface;
} ui_render_surface_desc;

// Payload of UI_MSG_CAPTURE; owned by the requester, who waits for `done`.
typedef struct ui_capture_request {
    ui_handle surface;
    ui_image *image;
    ui_result result;
    _Atomic bool done;
} ui_capture_request;

typedef struct ui_render_desc {
    const ui_logger *log;
    ui_hooks *hooks;
    ui_gpu_preference gpu;
    ui_queue *core_queue;    // FRAME_DONE / RENDER_FAILED go here
    ui_signal *progress;     // notified when a capture completes
} ui_render_desc;

typedef struct ui_render ui_render;

// No GPU work happens here; the device is created in the actor's start().
ui_result ui_render_create(const ui_render_desc *desc, ui_render **out);
void ui_render_destroy(ui_render *r);
ui_actor *ui_render_actor(ui_render *r);
