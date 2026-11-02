# Decisions log

Choices the architecture document leaves open, recorded as they are made. Newest
milestone first.

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
