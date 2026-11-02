// The core actor: applies command buffers at frame boundaries, ticks animations on the
// injectable clock, and commits one immutable display list per surface and frame.
// Runs on the main thread under both schedulers.
#pragma once

#include "core/clock.h"
#include "core/display_list.h"
#include "core/scene.h"
#include "hooks/hooks_internal.h"
#include "platform/platform.h"
#include "runtime/actor.h"
#include "wire/wire.h"
#include "ui/app.h"

typedef struct ui_core ui_core;

typedef struct ui_core_surface {
    ui_core *core;
    ui_handle handle;            // null when the slot is free
    ui_surface_kind kind;
    ui_psurface *platform;
    ui_display_list lists[3];
    ui_mailbox mailbox;          // lists[] cycle through it; render reads the published one
    ui_surface_config config;
    bool configured;
    bool resized_pending;        // a surface_resized event is owed to logic (size or scale)
    bool config_changed;         // a new configure must be acked with a drawn frame
    bool force_draw;             // redraw even if nothing changed (a frame was lost)
    // Pacing state. ready: the platform allows drawing now. awaiting: a frame callback is
    // outstanding (from the last drawn frame or an explicit request). Neither: idle; no
    // callbacks are requested until work arrives.
    bool frame_ready;
    bool awaiting_frame;
    uint64_t predicted_host_ns;
    uint64_t next_frame_id;
    uint64_t last_done_frame;    // last frame the render actor finished (presented or skipped)
} ui_core_surface;

typedef struct ui_core_desc {
    const ui_logger *log;
    ui_hooks *hooks;
    ui_platform *platform;
    ui_time_source time;
    bool start_paused;
    ui_signal *progress;         // notified on every message to the core
} ui_core_desc;

typedef struct ui_core_timer {
    uint32_t id;
    uint64_t deadline_ns;        // animation clock
} ui_core_timer;

#define UI_MAX_TIMERS 4096u

struct ui_core {
    ui_actor actor;              // its bounded queue carries render -> core messages
    ui_queue from_logic;         // unbounded: command buffers and EVENTS_DONE are never refused
    const ui_logger *log;
    ui_hooks *hooks;
    ui_platform *platform;
    ui_signal *progress;
    ui_actor *logic;             // destinations, set by the app before starting
    ui_actor *render;

    ui_clock clock;
    ui_scene scene;
    ui_handle_table surface_handles;
    ui_core_surface surfaces[UI_MAX_SURFACES];

    struct ui_pending_cmds {
        uint8_t *data;
        size_t size;
    } *pending;                  // command buffers received, applied at the next frame
    uint32_t pending_count, pending_capacity;

    ui_core_timer *timers;       // armed timers, unordered
    uint32_t timer_count, timer_capacity;

    ui_wire_buf events;          // events not yet sent to logic
    uint32_t events_in_flight;   // event buffers logic has not finished
    uint64_t last_anim_ns;

    bool quit_requested;
    ui_result failure;           // set when an actor reports a fatal error
    ui_app_stats stats;
};

ui_result ui_core_init(ui_core *c, const ui_core_desc *desc);
void ui_core_destroy(ui_core *c);

// The platform listener that routes callbacks into this core.
ui_platform_listener ui_core_platform_listener(ui_core *c);

// Creates a surface: platform object, scene root, and its registration with the render
// actor. Call after the platform is created and before the actors start.
ui_result ui_core_add_surface(ui_core *c, const ui_surface_desc *desc, ui_handle *out);
// Destroys all surfaces' platform objects. Call after the render actor has stopped.
void ui_core_remove_surfaces(ui_core *c);

ui_core_surface *ui_core_surface_get(ui_core *c, ui_handle handle);

// Marks every configured surface ready to draw now (headless frames, ui_app_frame).
void ui_core_force_frame(ui_core *c);

// Actor step: drains messages, produces a frame if any surface is ready, flushes events.
void ui_core_step(void *self);

// How long the main thread may block in the platform wait: 0 when a step has work now (a
// surface is ready to draw or messages wait), the time to the next timer, or UINT64_MAX.
uint64_t ui_core_wait_timeout_ns(ui_core *c);

// True when every surface's last committed frame is done and logic has handled all events.
bool ui_core_idle(const ui_core *c);
// True when the render actor has finished every committed frame (logic may still be busy).
bool ui_core_frames_done(const ui_core *c);
