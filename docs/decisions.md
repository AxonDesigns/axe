# Decisions log

Choices the architecture document leaves open, recorded as they are made. Newest
milestone first.

## Milestone 0.2: hardening before M1

### Idle frames
- **A surface draws only when it has work.** Work is any of:
  - an applied op on a node under it;
  - a spring under it that was running at the start of the frame (the settling frame still
    draws);
  - a size or scale change;
  - a new configure;
  - a forced redraw.
  A ready surface with none of these commits nothing, increments
  `ui_app_stats.frames_skipped_idle` and emits the `UI_TRACE_FRAME_IDLE` hook.
- **"Forced" means the renderer lost the frame** (no swapchain image), so the content must
  be redrawn even though nothing changed. `ui_app_frame` is *not* a force. It gives every
  surface a frame opportunity, like a vblank, so the headless test can show that frames
  without commands commit nothing.
- **Three pacing states per surface:** *ready* (may draw now), *awaiting* (a frame
  callback is outstanding) and *idle* (neither). A drawn frame asks for its next callback
  in its commit. An idle frame asks for none, so an idle window costs the compositor,
  the GPU and us nothing.
- **Waking up:** when an idle surface gets work, the core calls `surface_request_frame`
  once. On Wayland that is `wl_surface_frame` plus a `wl_surface_commit` with no new
  buffer, issued from the main thread. That is safe because an idle surface has no frame in
  flight and the render thread has nothing pending for it. The next frame is paced by the
  compositor's callback, as before the idle period.
- **Pending command buffers count as work for every idle surface.** Which surface a buffer
  touches is only known once it is applied at a frame boundary, so all of them are woken.
  A surface that turns out unaffected skips one frame. That costs one buffer-less commit
  per flush while idle, which is cheap next to an always-on loop.
- **A configure that changes only window state still draws one frame.** That is what acks
  it, so the compositor never waits on an unacked configure.

### Timers (new wire op and event)
- **Why:** idle frames remove the only clock logic had. Logic used `frame_committed` to time
  its every-2-seconds spring, so an idle window would stop the demo.
- **What:** command `set_timer(id, deadline_ns)`, opcode 7, and event
  `timer_fired(id, deadline_ns, now_ns)`, opcode 4, both on the animation clock. This is the
  architecture's pattern for async results: a request ID completed by an event, with no
  callbacks into the language. Old readers skip the new opcodes, so the wire version stays
  0.
- **Determinism:** timers are on the animation clock, so a paused or stepped clock drives
  them in tests. They fire in `ui_core_step` before the frame, earliest deadline first,
  ties by id.
  - In run mode the main thread sleeps in the platform wait exactly until the next due
    timer (`ui_core_wait_timeout_ns`, using the clock's inverse mapping), or forever while
    paused. The wait rounds up to whole milliseconds so it never spins early.
- **`set_timer` applies at the frame boundary like every command.** While idle it is one
  of the pending buffers that wake the surfaces (above).
- **Limit:** 4096 armed timers per app; more are dropped and counted as dropped ops.
- The demo arms its timer from the first `frame_committed` and re-arms from each
  deadline, so the period doesn't drift.

### Backpressure
- **Logic → core is a separate, unbounded queue** (`ui_core.from_logic`). It grows by
  doubling and never returns `UI_ERR_FULL`. The core's own bounded queue now carries only
  render → core messages (frame done, render failed), and core → logic stays bounded with
  coalescing.
  - **Why unbounded rather than blocking:** it needs no cross-scheduler wait protocol, and
    the single scheduler, where core and logic share a thread, can't block at all.
  - **Why memory is still bounded in practice:** logic can only run ahead by as many
    flushes as it makes between two core steps.
- **The sleep-polling retry loop is gone.** The only remaining failure is out of memory,
  and it loses nothing:
  - the logic actor keeps an ordered outbox and retries it before handling new events;
  - the core reserves room for every queued buffer before taking any, and on failure
    leaves them queued until the next step.
  So order and completeness hold even under memory pressure.
- **The flood test** pushes into `from_logic` directly, from another thread under the
  threaded scheduler. That is the exact channel the logic actor uses, and the logic host
  can't yet flush several buffers in one callback.

### Refresh-aware retry
- **A skipped frame now retries after the surface's own period**
  (`surface_period_ns`, a new platform call), instead of a hard-coded 16.67 ms. The period
  is:
  - on Wayland, the refresh from presentation feedback, else the measured present
    interval, else 60 Hz;
  - on headless, a nominal 60 Hz.
  A retry is also a forced redraw.

### Test-only frame without logic
- **`ui_app_frame_without_logic`** (internal, in `runtime/app_internal.h`) is a scheduler op
  that produces a frame and waits only for render to finish it, not for logic. The stall
  test uses it to keep frames coming while logic is held, without real sleeps.
  - Under the single scheduler it steps core and render and skips logic.
- **The stall test holds logic on a condition variable** inside its event callback for 32
  frames × 16 ms (512 ms of stepped clock time). Every frame must commit and the rect must
  follow the closed-form spring within 1.5 px. After release, the `set_color` written
  during the stall must apply.

### Tooling
- **The `UI_TRACE` printer is idle-aware.** It restarts interval measurement after an idle
  period, so an idle gap is no longer reported as a long frame or a late present, and it
  prints the number of idle frames.
- **`spring_rect --period S`** sets the time between springs, so idle behavior can be
  observed (long idle stretches with `--period 20`).

## Milestone 0.1: runtime skeleton

### Dependencies and build
- **wgpu-native v29.0.1.1** is pinned as a git submodule. Its crates are vendored by
  `tools/vendor-rust.sh` into the gitignored `third_party/wgpu-native-vendor/`, about
  215 MB. `.cargo/config.toml` makes every Cargo build offline against them, and
  `Cargo.lock` pins versions and checksums.
- **Corrosion v0.6.1** is copied in-tree; it is pure CMake.
- **Rust 1.93** is pinned to match wgpu-native's toolchain file.
- **wgpu-native is always built in release**, even in debug presets: a debug wgpu cannot
  hold frame rate, and we don't debug it. Only the `vulkan` and `wgsl` features are
  enabled on Linux.
- `_DEFAULT_SOURCE` is defined for the library because strict `-std=c23` hides POSIX
  interfaces such as `clock_gettime`.
- The WGSL shader is embedded with C23 `#embed`, so no build step is needed.

### Handles and wire
- **The logic side allocates node handles.** The core mirrors the generational scheme
  and validates each handle: `create_node` needs a free slot with a newer generation, and
  every other op needs an exact match. This avoids a round trip ("the core never waits on
  JS") and keeps stale-handle detection exact.
- **The top 4 bits of a handle's index carry its kind** (node or surface). A surface handle
  can never be confused with a node handle.
- **There is no `insert_child` yet.** `create_node` takes a parent, which is either a node
  or a surface (meaning a root-level node). Surfaces reach logic through `surface_resized`.
- **`destroy_node` destroys the subtree.** The logic side is responsible for freeing the
  descendants' handles in its allocator.
- **Malformed framing rejects the whole buffer**, which is what keeps flushes atomic.
  Semantically invalid ops (stale handles, NaN) are dropped one by one.
- **At most two event buffers are in flight to logic**; later events coalesce. A stalled
  logic actor therefore can't grow an unbounded queue.

### Threading and schedulers
- **"Identical behavior" between schedulers is defined for `ui_app_frame`**, which
  produces one frame and then waits until render has finished it and logic has handled
  its events. Under a paused clock the headless test requires byte-identical images from
  both schedulers.
  - In free-running `ui_app_run` mode, a threaded flush can land one frame later than in
    single mode. The Visibility guarantee ("no earlier than the next commit") allows this.
- **Single-scheduler order is core → logic → render.** Commands that logic produces in
  response to frame N apply at frame N+1 in both modes.
- **Closing a window ends the app (v0 policy).** Logic still receives
  `surface_close_requested` first. A real close protocol comes with the language runtime.
- **The render-thread check uses a thread-local scope counter**, set while the render
  actor's start, step or stop runs. Any `wgpu*` call outside it aborts in debug builds.
  This is the only thread-local state; it holds no app data.

### Rendering
- **Swapchain format:** BGRA8 sRGB (preferred) or RGBA8 sRGB, so blending happens in
  linear light. Pipelines for both are precompiled at startup.
- **Present mode is FIFO.** At most one frame is queued, through wgpu-native's
  `desiredMaximumFrameLatency = 1` (an extension, behind a capability) and through pacing
  on frame callbacks.
- **Headless targets are RGBA8 sRGB.** Captures read back the last rendered frame.
- **Display lists hold physical pixels and premultiplied linear colors.** The renderer
  uploads them directly as instances.
- **One animation time per frame**: the earliest predicted present among the surfaces
  drawing. Sampling per surface arrives with multi-monitor work.

### Wayland
- **Per-commit state is attached on the render thread** right before present: the
  configure ack, the viewport destination, the frame callback and presentation feedback.
  It must share the commit with the buffer of the matching size, which only the render
  thread knows. libwayland requests are thread-safe, and the callbacks are dispatched on
  the main thread.
- **Callbacks are created through a `wl_surface` proxy wrapper pinned to the default
  queue.** The NVIDIA Vulkan WSI moves the original `wl_surface` proxy to its private queue.
  Without the wrapper, our frame callbacks were never dispatched.
- **Predicted present time** = the first vblank after the frame callback (from the last
  presentation timestamp and the refresh period) plus a learned number of periods. That
  number moves after 4 consecutive presents that are more than half a period late or
  early.
  - With refresh 0 (VRR, or wlroots' headless backend), the period is measured from
    presents.
- **Versions bound:** seat v7 and compositor v4. This needs no handlers for events newer
  than libwayland 1.20 (pointer `axis_value120`, `warp`, `preferred_buffer_scale`).
- **The first frame renders at scale 1.** The compositor only sends `preferred_scale` after
  the surface is mapped on an output; the second frame is correct.
- The app ID is `dev.axe.app` for now.

### Testing
- The demo logic is a library shared by the example and the headless test. The test
  drives the real logic rather than a copy.
- The ASan test preset sets `VK_LOADER_DISABLE_DYNAMIC_LIBRARY_UNLOADING=1`. Without it,
  driver leaks show up as `<unknown module>` frames that `tools/lsan.supp` can't match.
