# Decisions log

Choices the architecture document leaves open, recorded as they are made. Newest
milestone first.

## Milestone 1: text pipeline

### CI
- **GitHub Actions, Linux only** (`.github/workflows/ci.yml`): `ubuntu-24.04`, one job per
  preset (`debug`, `asan`). Every test runs headless on lavapipe. `VK_DRIVER_FILES` names
  only the lavapipe ICD, so a test can never pick up a GPU by accident.
- **Clang 22 from apt.llvm.org** is pinned in CI (`LLVM_VERSION`). It is the oldest version
  the code was developed with. Local development may use newer versions.
- **What is cached.** Two caches, both keyed on the wgpu-native submodule commit:
  - the vendored crates, `third_party/wgpu-native-vendor/`;
  - Corrosion's Cargo target dir, `build/<preset>/cargo`, whose key also includes the Rust
    and LLVM versions.
- **What a warm run rebuilds.** Vendored crates are fingerprinted by version, so they never
  rebuild. Fresh checkouts give the `wgpu-native` crate itself new mtimes, so only that one
  crate rebuilds.
- On failure, the test log and any golden-diff PNGs are uploaded as artifacts.

### Unicode layer
- **Version: Unicode 18.0.0.** This is the UCD that the pinned HarfBuzz 15.0.0 ships
  (`hb-ucd-table.hh`). HarfBuzz's built-in UCD drives shaping and our tables drive
  segmentation, bidi and script runs, so the two never disagree about a character.
  `tools/unicode/fetch.sh` pins the source files by sha256.
- **Tables are generated and committed:** `tools/unicode/gen_tables.py` (Python 3, stdlib,
  run as `python3 -I`) writes `src/unicode/ucd_tables.{h,c}`.
  - Each code point maps to one 8-byte record: line break class, bidi class, grapheme
    break, general category, script, flags (Extended_Pictographic, East Asian, InCB,
    Default_Ignorable) and a Script_Extensions index.
  - 1,154 distinct records are found through a two-stage table with blocks of 128, about
    110 KB in all.
  - Enum values are fixed in the generator, not taken from file order, so a version bump
    doesn't renumber them.
- **The algorithms work on code point arrays.** Callers keep a parallel byte-offset array,
  so every later stage still speaks UTF-8 byte offsets.
  - Line breaking and bidi take a reusable context and allocate only when they grow.
  - Grapheme breaking allocates nothing.
- **Conformance: 100%, no deviations, nothing skipped.** BidiCharacterTest (91,707 cases),
  LineBreakTest (19,346) and GraphemeBreakTest (853) run as the CTest tests
  `unicode_bidi`, `unicode_linebreak` and `unicode_grapheme`. The files are committed under
  `tests/unicode/` (about 10 MB, Unicode License v3) so tests run offline.
- **Changes in 18.0 that the code follows:**
  - LB12a no longer allows a break between BA and GL.
  - U+00AD is HH, and U+2012/U+2013 are BA.
  - GB9c is `\p{InCB=Linker} \p{InCB=Extend}* × \p{InCB=Consonant}`.
- **Line breaking groups LB9 units first.** A base character plus the combining marks it
  absorbs is one unit, and the rules are then evaluated in order between adjacent units.
  - A CM that starts a unit becomes AL with the properties of U+0041 (LB10).
  - SA characters resolve by general category (LB1). There is no dictionary breaking for
    Thai, Lao, Khmer or Myanmar, so those lines break only at spaces and punctuation.
- **Bidi keeps removed (X9) characters in place and skips them.** Level runs and isolating
  run sequences are lists of kept indices.
  - I1–I2 run once per paragraph after all sequences, so sos and eos see explicit levels.
  - Removed characters render at the level of the character before them.
  - BD16 pairs are unbounded per sequence; only the 63-deep opening stack is limited, as
    the spec requires.
- **Script runs (UAX #24):**
  - Common and Inherited join the surrounding run; leading ones join the first real script.
  - Script_Extensions narrow a run's candidate set. A character starts a new run only when
    the intersection would be empty.
  - A closing bracket takes its opener's script.
  - All-Common text resolves to Zyyy.
- **UTF-8 policy:** one U+FFFD per maximal subpart (Unicode §3.9, as in WHATWG).
  `ui_utf8_sanitize` returns the replacement count, which the wire counts in stats.

### HarfBuzz, Fontconfig and the test fonts
- **HarfBuzz 15.0.0** is a git submodule. A project-owned `third_party/harfbuzz.cmake`
  compiles the amalgamated `harfbuzz.cc` with Clang++, using upstream meson's flags
  (`-fno-exceptions -fno-rtti -fno-threadsafe-statics`).
  - `HAVE_PTHREAD` makes it use pthread mutexes rather than `std::mutex`.
  - There is no FreeType, glib, ICU, graphite or platform shaper: HarfBuzz's own OpenType
    functions load fonts, shape and draw outlines.
  - `harfbuzz-subset.cc` is a separate target, used only by `tools/font_subset`.
  - C++ is enabled project-wide (`CMAKE_CXX_COMPILER=clang++` in the presets) but compiles
    only HarfBuzz. The sanitizer presets instrument it too.
- **One module per dependency.** `src/text/hb/` is the only code that includes `hb.h`, and
  `src/text/fontconfig/` the only code that includes `fontconfig.h`. Neither leaks a type
  into another header.
- **Shaping settings.**
  - Cluster level `MONOTONE_GRAPHEMES`, so HarfBuzz clusters align with grapheme
    boundaries, which hit testing needs.
  - Clusters are UTF-8 byte offsets into the whole paragraph. The surrounding text is
    passed as context, so joining across run boundaries is correct.
  - Fonts are scaled to `size_px * 64` with no ppem, so there is no hinting.
  - Language is HarfBuzz's default. A span language field is a follow-up.
- **Fontconfig** (minimum 2.13) is used only with `text.system_fonts`, or with extra font
  directories.
  - Tests build an isolated configuration (`FcConfigCreate` + `FcConfigAppFontAddDir`)
    that knows only `tests/fonts`. It sets the XDG cache directory so Fontconfig doesn't
    warn.
  - `ui_fc_match` accepts a result only if its family really matches, because
    `FcFontMatch` always returns something. Generic families (sans-serif, serif,
    monospace, system-ui) resolve to whatever Fontconfig picks.
  - Variable fonts' named-instance index bits are dropped: axes are set from the span's
    weight and style.
- **Font matching** follows CSS Fonts 4 §5.2.
  - The weight distance: 400–500 looks up to 500, then down, then up; lighter weights
    look down first; bolder weights look up first. A variable `wght` axis matches any
    weight in its range.
  - A style mismatch costs more than any weight difference.
  - There is no synthetic bold or italic. An italic request uses an `ital` axis, else
    `slnt` (down to -12), else an italic face.
- **Fallback per grapheme cluster.** The primary face is tried first, then the span's
  family list, then registered families in registration order (each at its best weight
  and style), then Fontconfig fallback (cached by the cluster's first non-ignorable code
  point). If nothing covers the cluster, the primary face draws `.notdef`.
  - Default-ignorable code points need no coverage.
  - **Stickiness:** a Common or Inherited cluster (space, punctuation, digits) keeps the
    previous cluster's font when that font covers it. Otherwise every space inside fallback
    text would split the run and lose kerning.
- **Runs** split where the bidi level, script, resolved face or shaping style (family,
  weight, style, size) changes. Color is not a split: glyph color comes from the style of
  the glyph's cluster.
- **Test fonts:** `tests/fonts/`, 3.8 MB, OFL, pinned to a google/fonts commit with sha256
  (`tests/fonts/SOURCES.md`).
  - Noto Sans (variable wght/wdth; it also covers Devanagari), Noto Sans Hebrew, Arabic and
    Devanagari.
  - Noto Sans SC subset to `cjk_chars.txt` (249 KB instead of 17.8 MB). The subset tool
    keeps every name record: `hb-subset` normally keeps only IDs 0–6, which renamed the
    font "Noto Sans SC Thin".
  - The subset is a Modified Version under the OFL. Its name doesn't use the declared
    Reserved Font Name "Source".

### Rasterizer and atlas
- **Coverage rasterizer** (`src/text/raster.c`). Outlines come from HarfBuzz's draw API
  and are flattened adaptively: the segment count bounds the chord error |B''|h²/8 by
  1/16 px, up to 128 segments per curve.
  - Each line segment adds its exact trapezoid area to an accumulation buffer, in the
    scheme of font-rs and stb_truetype 2. A row prefix sum then gives coverage =
    min(1, |winding area|).
  - This is exact for non-overlapping contours and for same-direction overlaps (variable
    fonts overlap contours this way). Opposite-direction overlaps cancel.
  - There is no supersampling. Tests check a 10×10 square at quarter-pixel offsets to
    within 8-bit quantization, and a circle (cubic and quadratic) to within 0.5% of πr².
  - Bounds come from the flattened segments, not from HarfBuzz extents, so a bitmap is
    never clipped.
  - Bitmaps larger than 4096 px per side are refused.
- **Subpixel positioning:** 4 horizontal bins (0, ¼, ½, ¾ px), each rasterized at its
  offset. There is no vertical bin because baselines snap to whole pixels.
- **Cache key:** face, interned variation instance, physical size in 1/64 px, glyph ID and
  bin. The table uses open addressing (linear probing with tombstones) and rehashes at
  75% load.
- **Pages:** 1024² R8 by default, up to 8 (`ui_atlas_desc`). Each page op carries a
  format, so RGBA pages for color glyphs need no protocol change.
  - Shelf packing: shelf heights round up to 4 px, and a glyph takes the shortest shelf
    within +25% of its height. Freed columns are kept sorted and merged, and space next
    to a shelf's bump pointer returns to it.
  - An empty trailing shelf gives its rows back to the page.
  - Glyphs need no padding because the shader reads texels with `textureLoad` at exact
    integer positions.
  - Glyphs larger than a quarter page are rejected and counted.
- **Eviction.** A glyph is evicted once it has gone unused for `evict_frames` epochs
  (default 300). An epoch is one core frame that draws, so an idle app doesn't age glyphs.
  - A glyph used at or after the protect epoch is never evicted. The protect epoch is the
    oldest epoch among the surfaces' newest unfinished display lists.
  - Under pressure (no space and no new page allowed), every glyph that is not used this
    frame and not protected is dropped at once, then placement is retried. Dropping
    everything is blunt but simple and correct, and pressure needs a full atlas.
  - A glyph that still doesn't fit is cached as empty (drawn as nothing, counted as
    rejected), so it isn't retried every frame. It ages out like any other entry.
  - Pages after the first are released once empty for the eviction window.
- **Uploads survive mailbox replacement.** Atlas changes travel in `UI_MSG_ATLAS_UPDATE`
  messages (one per frame, increasing `seq`) through the render actor's inbox. That inbox
  is now **unbounded**, so it can't refuse or drop one.
  - The core pushes a frame's update before publishing that frame's lists, and stamps
    each list with `atlas_seq`.
  - The render actor handles its inbox only between lists. After taking a list, it applies
    queued updates until `atlas_seq` is reached, then draws.
  - An update that belonged to a replaced (undrawn) list is still in the FIFO and is
    applied before the next list.
  - Every texture write is therefore queued after any submitted draw that could read the
    old texels. With the protect epoch, no list still to be drawn references an evicted
    region.
  - A list whose `atlas_seq` the inbox can't satisfy is a protocol violation. It is logged
    and drawn anyway.

### Paragraph layout
- **Greedy line breaking** over UAX #14 opportunities, measured with the shaped advances.
  - Each code point's advance is the sum of its cluster's glyphs; with grapheme-level
    clusters, the advance sits on the cluster's first code point.
  - A line ends at a mandatory break, else at the last opportunity before the content
    overflows, else (no opportunity: a word wider than the line) at the last grapheme
    boundary before the overflow, with at least one grapheme per line.
  - A width within 1/64 px of the limit counts as fitting.
- **Trailing whitespace hangs.** SP, the hard break characters and ZWSP at the logical end
  of a line are laid out but excluded from the line's width, so they never cause a break.
  In RTL lines they sit left of the content.
- **Pieces and reordering.** A line is cut into pieces wherever the shaped run or the L1
  level changes, and L2 then orders the pieces.
  - Within a piece, glyphs keep HarfBuzz's visual order. A piece whose L1 level parity
    differs from its run's (whitespace reset to the paragraph level) is reversed.
  - A piece that starts or ends mid-run is sliced by cluster when HarfBuzz marks the
    boundary safe. Otherwise it is reshaped, with the paragraph as context, so cursive
    joining continues across an intra-word break (CSS Text 3 §7.3).
- **Metrics:**
  - The line box height is `round(max ascent + max descent + max line gap)` over the
    line's runs, with the gap split evenly above and below.
  - Baselines are `round(top + gap/2 + ascent)`, and line tops accumulate rounded
    heights, so spacing is uniform and every baseline is a whole physical pixel.
- **Alignment is start only:** left in LTR paragraphs, right in RTL ones. Each line uses
  its own paragraph's level, since texts with paragraph separators can mix directions.
  Unbounded layouts align to the widest line.
- **Max lines and the ellipsis.**
  - When `max_lines` cuts text off, the last kept line is shortened to the longest prefix
    that fits with the ellipsis, ending at a grapheme boundary and never on whitespace.
  - The ellipsis is shaped in the font and script of the run before it, at the paragraph
    direction, as a piece at the paragraph level at the logical end (on the right in LTR,
    on the left in RTL).
  - The default is U+2026; if that font lacks it, "..." is used. An empty ellipsis cuts
    without one.
- **Empty lines:** empty text is one empty line in the base font, and text ending in a
  hard break gets an empty last line. Carets need both once editing exists.
- **Cache:** `ui_paragraph` keeps the shaped paragraph (keyed by text, spans, direction
  and scale) and the line layout (also width and max lines). A width change, such as a
  resize or a spring, only re-breaks lines. `shape_count` and `layout_count` let tests
  check this.

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
