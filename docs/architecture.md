# UI Framework Architecture Plan

Nov 1, 2026 · @Brayan

## Overview

We are building a production-grade, cross-platform UI framework with a C core, TypeScript authoring and a WebGPU renderer. The first release targets Linux (Wayland and X11), Windows and macOS; the web is deferred, and Android and iOS follow in a later version, but nothing in the core may assume a desktop.

Priorities, in order:

1. Performance: frames are never blocked by app logic, at 60 to 144 Hz.
2. Production-grade text: shaping, bidi, fallback, editing and IME on every platform.
3. Smooth animation: springs, layout animations and gesture-driven animations.
4. Navigation: progress-driven route transitions, interactive gestures and hero widgets.
5. Developer and AI experience: HMR, devtools, headless mode and an automation protocol usable over MCP.

The guiding rule is no shortcuts: decisions that are expensive to retrofit (threading, accessibility, IME, automation, determinism) are made and built from the first milestone.

## Decision record

| Area | Decision |
| --- | --- |
| Core language | C23 built with Clang everywhere, linking a C++ runtime (HarfBuzz, ThorVG) and Rust crates (wgpu-native, AccessKit) |
| Threads | Core (OS main thread), logic (JS), render (sole GPU owner); collapsible to one thread |
| JS engine | QuickJS-ng behind a strict engine interface |
| Authoring | Language-agnostic C API and wire protocol; TypeScript first (fine-grained signals, TSX), Dart possible later; retained-mode scene, styles as objects |
| GPU | WebGPU through the standard webgpu.h, implemented by wgpu-native |
| Text | HarfBuzz shaping, own rasterizer, own Unicode algorithms, own layout, editing and IME |
| Layout | Own flex + grid engine, incremental |
| Vector assets | ThorVG for SVG and Lottie, rendered to textures, driven by the core clock |
| Accessibility | Own semantics tree; AccessKit for desktop bridges |
| Surfaces | One core, many surfaces |
| Linux | Wayland and X11 in the first release; layer-shell is Wayland-only |
| First vertical slice | Linux (Wayland) |
| Build | CMake, with Corrosion for Rust crates |
| Automation | One JSON-RPC protocol for devtools, tests and MCP; headless surfaces |
| Plugins | Single package: TS API, per-platform native code, IDL-generated bindings |
| License | Dual license (open source + commercial); open-source license still open; CLA required |
| Targets | Desktop only for now: Linux (Wayland and X11), Windows, macOS; web and mobile deferred |

## Threading model

&#91;embedded content: threading model · 3 threads, one-way data flow\]

Each app instance runs three threads, built as actors with message queues. The core thread is the OS main thread because macOS and Win32 require window and event work there. Display lists are double-buffered: if the render thread falls behind, the core replaces the pending list instead of queueing it.

The same actors run under two schedulers: threaded, and single-threaded where all three stages run in order each frame. Both are supported from the first milestone.

## Threading contract and ordering guarantees

The core never waits on JS, so every API where JS expects a synchronous answer from the core is redesigned as declarative configuration or an asynchronous result.

Guarantees the framework documents and tests:

- **Visibility.** A signal write becomes visible on screen no earlier than the next frame commit after the logic thread flushes its command buffer. Writes within one flush are applied atomically.
- **Event order.** Input events reach JS in the order the core received them, each stamped with the frame it was dispatched in.
- **Destroyed nodes.** Commands addressed to a destroyed node's stale handle are dropped and reported in debug builds, never undefined behavior.
- **No partial frames.** The render thread only ever sees complete, immutable display lists.

APIs redesigned because of this contract:

- **Layout reads.** Measurement is asynchronous: results arrive as a signal on the next frame. Most needs are covered declaratively instead: anchoring, popovers, tooltips and fit-to-space positioning run inside the core's layout.
- **Gesture consumption.** There is no synchronous preventDefault. Nodes declare recognizers, blocking relationships and arena priorities up front; JS handlers are notified after decisions.
- **Text input.** The core owns editing state (text, selection, IME composition). JS observes changes and may apply transformations; an edit counter detects stale JS updates, which are rebased instead of overwriting newer typing.
- **Errors.** A JS exception never stops the core. Component error boundaries contain failures; the core keeps rendering the last valid scene, and HMR failures show an overlay.

## App and surface model

One app instance (the core) owns many surfaces. A window is one kind of surface.

| Owner | Owns |
| --- | --- |
| App (core) | Signals runtime, scene graph, style and theme resolution, font, glyph, image and shader caches, the GPU device, the global animation clock, navigation state |
| Surface | Root node, swapchain, scale factor and color space, input routing and focus, its own frame schedule |

Each surface schedules frames for its own display, so a 60 Hz and a 144 Hz monitor run side by side. The animation clock stays global; each surface samples it at its own predicted present time.

Surface kinds: top-level window, popup or menu, Wayland layer-shell surface, and headless surface. Portals render a subtree into another surface. Hero transitions across surfaces are unsupported in v1.

Platform operations that a platform cannot perform (setting a window position on Wayland) return an explicit unsupported result rather than failing silently.

## JS engine interface and FFI

QuickJS-ng is the default engine, chosen for spec compliance, small size, a plain C build and an MIT license. It has no JIT, which is acceptable because animation, layout, scrolling and text run in C. JavaScriptCore or V8 can be added later as optional backends if profiling shows JS is the bottleneck.

**Engine interface.** The framework talks to engines only through its own interface: values, objects, functions, modules, GC handles, microtasks and exceptions. No engine type appears outside the adapter. Gaps in an engine are filled above the interface; QuickJS-ng has no Intl, so the framework provides its own on top of its Unicode layer.

**Command buffer.** TS never calls C per property. It writes operations (create node, set property, insert child, start animation) into a buffer that the core applies at the next frame boundary.

**Handles.** C objects are referenced by generational handles (index + generation), never raw pointers. A stale handle is detected, not dereferenced.

**Bindings.** All TS to C bindings are generated from an IDL, for the framework itself and for packages.

**TypeScript.** esbuild or SWC transpiles in development; release builds compile ahead of time and ship QuickJS bytecode for faster startup.

## Language bindings

The framework is defined by a language-agnostic C API and wire protocol; TypeScript is the first binding, not the framework. A Dart or Luau binding is additional work, not a rewrite, and plain C apps work for free.

| Layer | Contents | Per language? |
| --- | --- | --- |
| Core C API | Surfaces, nodes, properties, styles, animations, gesture configuration, transitions, text, assets, semantics, automation | No: stable C ABI |
| Wire protocol | Command buffer (language to core) and event buffer (core to language), schemas in the IDL | No: generated encoders and decoders |
| Runtime host interface | Event-loop integration, handle finalization, module loading and hot reload hooks, error and devtools reporting | Once per runtime |
| Language framework | Idiomatic authoring model: signals and TSX for TypeScript, idiomatic Dart for Dart | Yes |

Design rules:

- **Two buffers, both directions.** No synchronous callbacks from C into a language on the frame path; a language only reads and writes buffers.
- **No language semantics in the C API.** No promises, closures or objects. Asynchronous results are request IDs completed by events, wrapped as Promises or Futures by each binding.
- **Signals stay in the language.** The reactive graph lives in each framework; the core sees property changes. Gesture- and scroll-bound animated values remain in the core. Signal-graph devtools data reaches the hook API through the runtime host interface.
- **Text offsets are UTF-8 byte offsets at the ABI.** JS and Dart strings are UTF-16, so bindings convert selection, caret and IME ranges at the edge.
- **Explicit ownership.** Handles are released by destroy commands; language finalizers are only a safety net.
- **Packages generate bindings for every language** from their IDL.

**Dart hosting.** The recommended model embeds the Dart VM on the logic thread, as Flutter does, so the core keeps the OS main thread. It brings Dart hot reload in development and AOT snapshots in release; the cost is a heavy VM build. Dart as host through dart:ffi conflicts with the core owning the main thread and is not planned.

**Keeping the layer honest.** The C test harness drives the C API and wire protocol directly from day one. The vertical slice adds a minimal throwaway second binding (Luau or Dart creating nodes and handling a tap) to expose leaks.

**Language benchmarks** run in the vertical slice, with thresholds set before running:

| Benchmark | Measures |
| --- | --- |
| Cold start of a screen with about 1,000 nodes | Time to first frame |
| 10,000-item list scrolling while JS updates items | Dropped frames, update delay |
| Text field with a JS input-transforming handler | Typing latency |

Outcomes: thresholds met, TypeScript on QuickJS-ng stays; compute-only failures are fixed with native compute modules; failures on the update path itself make Luau or Dart the candidate behind the same layer.

**Keeping work out of JS by design:** compile-time TSX transforms that turn static subtrees into single commands with interned styles; virtualized lists, tables and text editing in C; native compute modules exposed through the IDL; precompiled bytecode in release.

## Authoring model and styling

Apps are written in TypeScript with fine-grained signals over a retained-mode scene. A changed signal patches exactly the properties that depend on it; there is no virtual DOM and no diffing. Components are written in TSX with a custom compile transform.

Styles are typed objects:

- **Interned static styles.** Styles created at module level are registered once and referenced by ID, so applying one costs one command.
- **Dynamic properties** can be signals, updating a single property without touching the rest.
- **Composition** through arrays or merging, with defined precedence.
- **State variants** (hover, pressed, focused, disabled, selected) are resolved in the core, never through a JS round-trip.
- **Transitions declared in the style** animate property changes on the core thread.
- **Theme tokens** with light, dark and high-contrast variants; system preferences for reduced motion, text scale and accent color are respected.
- **Token scopes:** a subtree can give tokens values of its own (a style block's declarations, including state blocks, or the node's), like CSS custom properties but with aliases resolved where they are used. Themed regions and hover re-theming need no JS (docs/styles.md, "Token scopes").
- **Units:** logical pixels, percentages, fr, auto. Invalid styles fail at compile time.

## Rendering

All GPU work happens on the render thread through the standard webgpu.h, implemented by wgpu-native. Code targets the standard header only; implementation-specific extensions sit behind capability checks.

- **Primitives.** Rounded rects, borders, shadows, gradients and images use analytic signed-distance shaders in large instanced batches.
- **Paths.** Arbitrary paths are tessellated on the CPU first; a compute-based path renderer may follow.
- **Compositing layers.** Animated subtrees, opacity groups, blurs and backdrop filters render into cached textures, so transform and opacity animations are recomposites.
- **Damage tracking** limits redraws to changed regions.
- **Frame pacing** uses each platform's timing source: Wayland frame callbacks and presentation-time, the DXGI frame-latency waitable object, CVDisplayLink. Variable refresh rates are supported, and the animation clock receives the predicted present time.
- **Pipeline cache.** All pipeline variants are precompiled at startup to avoid shader-compile stalls.
- **Renderer-agnostic display list.** The display list does not depend on WebGPU, so a CPU reference renderer can be added later for deterministic golden tests and as a last-resort fallback.

## Text

HarfBuzz is the only text dependency; everything else is built in-house.

| Layer | Implementation |
| --- | --- |
| Shaping | HarfBuzz |
| Outlines and color glyphs | HarfBuzz draw and paint APIs (variable fonts, COLRv1); no FreeType |
| Rasterization | Own rasterizer into a glyph atlas, grayscale AA, subpixel positioning |
| Unicode algorithms | Own bidi, line breaking, grapheme and word segmentation, tables generated from Unicode data files |
| Font discovery and fallback | DirectWrite, CoreText, Fontconfig |
| Paragraph layout | Own: rich spans, ellipsis, max lines, baselines, hit testing |
| Editing | Own: selection, caret movement (including RTL), clipboard, undo |
| IME | TSF (Windows), NSTextInputClient (macOS), text-input-v3 (Wayland) |
| Intl | Own implementation on top of the Unicode layer |

IME is built alongside the first text input widget on Wayland, not added afterward. Font, glyph and IME entry points are fuzzed.

## Vector assets and images

**ThorVG** renders SVG and Lottie assets. It is an asset renderer, not the UI renderer: it draws into textures the compositor treats as layers.

- Runs on the render thread with the framework's GPU device, through its WebGPU canvas. Its worker thread count starts at zero.
- Never plays itself: the core sets each frame's playhead from time, a spring, a scroll position or a gesture. Pausing, slow motion, wait-for-idle and headless screenshots therefore work unchanged.
- Static SVGs are rasterized once per size and scale factor and cached.
- Scope: static SVG for icons and illustrations, Lottie for animation. SVG SMIL and CSS animations are out of scope.
- Vendored with a project-owned CMake file building only the Lottie, SVG and WebGPU modules.

**Raster images** go through the framework's own codec layer, not ThorVG's.

| Format | Decoder |
| --- | --- |
| BMP, ICO, GIF (animated), QOI | Own |
| PNG, APNG | libspng or libpng |
| JPEG | libjpeg-turbo |
| WebP (animated) | libwebp |
| AVIF | libavif + dav1d |
| JPEG XL | Optional |
| HEIC | Platform decoders only (ImageIO, WIC); not bundled due to HEVC patents |

All formats decode off-thread, downsample during decode, honor EXIF orientation and ICC profiles, support Display P3, use a memory-budgeted cache, and enforce hard limits on dimensions and memory.

## Layout

The framework builds its own flex and grid engine, running on the core thread.

- **Incremental.** Dirty flags and measurement caching, so a text change relayouts only the affected subtree.
- **Measure callbacks** for text and images; baseline alignment.
- **Declarative positioning** for anchoring, popovers and tooltips, so apps rarely need to measure.
- **Layout vs presentation.** Each node has a layout rect and a presentation rect. When layout changes, the core animates the presentation transform from old to new (FLIP-style). Layout animations and hero flights share this mechanism.
- **Offscreen layout pass** for incoming routes, so transitions know destination rects before they start.

## Input, gestures and scrolling

Touch, pen and trackpad are first-class on every platform, not only on mobile.

**Unified pointer event:** type (mouse, touch, pen, eraser), ID, position, buttons, pressure, tilt, twist, contact size, hover state, plus coalesced samples and predicted positions.

| Platform | Input sources |
| --- | --- |
| Wayland | wl\_pointer (axis source, axis\_stop), wl\_touch, tablet protocol, pointer-gestures, xkbcommon |
| Windows | WM\_POINTER, DirectManipulation for precision touchpads |
| macOS | NSEvent (precise deltas, momentum phases, magnify), tablet events, NSTouch |

**Gesture arena** in the style of Flutter: recognizers compete, resolve deterministically, and are configured declaratively. Velocity comes from a least-squares fit over recent samples.

**Scrolling** runs entirely on the core thread with iOS physics: exponential deceleration (UIKit rate 0.998 per ms), rubber-band overscroll with resistance (1 - 1/(x \* c / d + 1)) \* d, and spring-back. Discrete wheel notches animate toward an accumulating target with a critically damped spring. Nested scroll handoff, scroll snapping and animated programmatic scrolling are included.

**Touch and pen extras:** touch-sized hit slop, palm rejection while a pen is active, long-press and context menu, and a low-latency inking path drawn by the render thread from coalesced samples.

Windows precision-touchpad scrolling via DirectManipulation is a known high-effort item and is budgeted separately.

## Animation

All animations tick on the core thread from a single injectable clock.

- **Springs are the default.** They are interruptible and velocity-preserving: a new target mid-flight keeps the current velocity.
- **Timing curves and keyframes** for the remaining cases.
- **Animated values** bind properties to gesture or scroll values through a small expression graph evaluated in C, for example opacity = clamp(1 - dragY / 300). This covers gesture-driven UI without running JS per frame.
- **Worklets** (small functions in a second JS context on the core thread) are a later escape hatch.
- **Clock control:** pause, step and time scale, used by tests, devtools and the automation protocol.

## Routing and transitions

Every route transition is a core-owned object with a progress value from 0 to 1. Every animated aspect of the transition (page transforms, opacity, dimming, hero flights) is a function of progress, never of time.

- **Drivers set progress.** A spring or timing driver handles programmatic push and pop; a gesture driver maps finger position to progress. Drivers hand off mid-flight with the gesture's velocity, so there is no jump.
- **Commit or cancel** on release is decided by position and velocity: a fast flick commits at low progress, a slow drag back cancels. Cancel animates back to 0 and fully restores the previous route.
- **Lifecycle:** entering, active, exiting, cancelled. Both routes stay live and rendered during a transition; JS receives lifecycle events, but nothing on the frame path waits for JS.
- **Heroes.** Tags are matched across routes, the incoming page is laid out offscreen first, and both heroes fly in a surface-level overlay layer. Handled edge cases: heroes inside scrolled or clipped containers, offscreen targets, interruption mid-flight, nested navigators.
- **Gesture conflicts.** The back-swipe recognizer joins the gesture arena; edge regions win over inner horizontal scrollers.
- **Platform inputs** map onto the gesture driver: iOS edge swipe, Android predictive back, macOS two-finger swipe, mouse back buttons and keyboard shortcuts.
- **Interruption policy** allows pushes during interactive pops by default: the interrupted transition is retargeted from its current progress and velocity, never jumping. Configurable per navigator.
- **Also:** nested navigators, modals and sheets, per-surface stacks, deep linking, state restoration.

## Accessibility

Every widget contributes to a semantics tree maintained by the core alongside the scene. The same tree powers the automation protocol, so whatever an agent can drive, a screen reader can too.

- **Desktop bridges:** AccessKit (MIT/Apache) maps the semantics tree to UI Automation, NSAccessibility and AT-SPI.
- **Mobile:** a requirement. AccessKit ships an Android adapter and, since 2026, a basic iOS adapter; the framework fills any gaps with its own bridge code.

## Platform layer and windows

The framework writes its own thin platform layer per OS; SDL and GLFW do not cover custom decorations, layer-shell and IME control.

| Platform | Key requirements |
| --- | --- |
| Wayland (first) | libwayland, xkbcommon; xdg-shell, xdg-decoration, fractional-scale + viewporter, presentation-time, text-input-v3, pointer-gestures, tablet, cursor-shape; wlr-layer-shell with a capability query |
| Windows | WM\_NCHITTEST including HTMAXBUTTON for snap layouts, DWM shadows, per-monitor DPI |
| macOS | Full-size content view, repositioned traffic lights, native menu bar |
| X11 | First release: XInput2 for touch, pen and smooth scrolling; XIM or IBus for IME; Present extension for frame timing; \_NET\_WM\_MOVERESIZE for custom decorations; no layer-shell |

**Custom decorations** on all platforms: resize edges, window menu, double-click-to-maximize semantics per OS. Client-side decorations are mandatory on GNOME.

**Layer-shell** is unavailable on GNOME, so the API exposes a capability query and fails gracefully.

**DPI:** per-monitor and fractional scaling; moving between monitors invalidates glyph atlases and cached rasters.

**Also:** clipboard with rich formats, drag and drop (next section), file dialogs (XDG desktop portal on Linux), cursors, system tray and menus.

The first slice is tested on GNOME, KDE and a wlroots compositor (Sway).

## Drag and drop

One core-owned drag session serves drags within a surface, between windows of the same app and between apps, with the same API, data model and visuals in all three. It ships in the first release on every desktop platform and shares its data model with the clipboard.

| Scope | Who runs the session | Payload |
| --- | --- | --- |
| Within one surface | Core only, no OS session | The in-process value, never serialized |
| Between surfaces of one app | OS session, recognized as same-process on drop | The in-process value, resolved from a session token |
| Between apps | OS session | Serialized representations, read on drop |

**Promotion.** A drag starts in the core and is promoted to an OS session as soon as it can leave its surface: when the pointer exits, or at start for sources marked external. Wayland requires this, since the compositor owns the pointer outside the surface, and it keeps one path for our own windows and other apps' windows. Sources confined to their surface (list reorder, sliders, canvas tools) never promote.

**Data model.** A drag carries a list of items; each item offers several representations (MIME types, mapped to UTIs and Windows clipboard formats), using the same offer type as the clipboard.

- **Lazy data.** The source provides bytes only when a target asks on drop, through a request ID completed by an event, the architecture's pattern for async results.
- **File promises** for content that does not exist yet (an export, a remote file): NSFilePromiseProvider on macOS, FILEDESCRIPTOR/FILECONTENTS on Windows, a temp file offered as text/uri-list on Linux.
- **Session token.** Every drag also offers a private format naming the session; a target in the same process resolves it to the live value, so moving a model between windows keeps its identity and skips serialization.
- **Custom types** are registered with a schema: other apps on the framework read them as structured data, everyone else sees the fallbacks (text, URI list, image).

**Operations.** Copy, move, link and ask are negotiated between source and target, with the modifier keys of each OS. A move into another app completes only when the target confirms it (Wayland dnd\_finished, DROPEFFECT\_MOVE, NSDragOperationMove); the source deletes nothing before then.

**Threading.** Hit testing, target highlighting and preview motion run on the core thread at display rate. Drop targets declare accepted types and operations as data, so hover feedback never waits for JS. A target may add an async check in JS; until it answers, the declarative answer stands. Drops and data reads reach logic as ordinary events.

**Gestures.** The drag recognizer joins the gesture arena. Mouse drags start past the OS drag threshold; touch and pen drags start on long-press, so a quick swipe still scrolls. Inner scrollers yield once a drag is recognized.

**Visuals.**

- **Inside a surface** the preview renders in the overlay layer hero flights use, follows predicted pointer positions, lifts with a spring and lands on the target through the presentation-rect mechanism. A cancel springs it back to its origin.
- **Outside a surface** it becomes the OS drag image: a drag-icon wl\_surface on Wayland, drawn by the render thread so it stays live; an ARGB drag window on X11; IDragSourceHelper on Windows; NSDraggingItem image components on macOS.
- **Multi-item drags** show a stack with a count badge, and targets can restyle the preview on enter.

**Built-in behaviors.**

- **Edge auto-scroll** in scroll views, faster the deeper the pointer sits in the edge zone, driven by the existing scroll physics.
- **Spring-loaded targets:** hovering a tab, folder or nav item for 500 ms activates it, pushing a route through the normal transition.
- **Reorder** in lists and grids with a live gap; siblings move through layout animations.
- **Tear-off and dock:** dragging a tab out creates a window that follows the pointer (xdg-toplevel-drag-v1 on Wayland, behind a capability query; a framework-moved top-level elsewhere). Dropping it on another window's tab strip docks it.
- **Cancellation** on Esc, a lost pointer grab, the source surface closing, the target app vanishing or app quit; each ends the session cleanly and animates back where possible.
- **Several touch drags at once** inside the app; OS sessions are limited to one per seat.

| Platform | Mechanism |
| --- | --- |
| Wayland | wl\_data\_device start\_drag with the press serial, data offers with set\_actions, drag-icon surface, xdg-toplevel-drag-v1 for tear-off, FileTransfer portal for sandboxed apps |
| X11 | Own XDND v5 implementation, copy, move, link and ask actions, ARGB drag window |
| Windows | OLE IDropTarget per HWND (OleInitialize on the main thread), IDataObject with delayed rendering, DoDragDrop with IDropSource, shell drag-image helpers |
| macOS | NSDraggingSource and NSDraggingDestination on the content view, NSPasteboardItem data providers, NSFilePromiseProvider, spring-loading |

**Windows DoDragDrop** runs a modal message loop on the main thread. The core keeps stepping from inside that loop, so frames, animations and logic events continue during an outbound drag. This is a known high-effort item, budgeted separately like DirectManipulation. UIPI blocks drops from a normal app into an elevated one; this is documented, not worked around.

**Accessibility and keyboard.** Drag sources and drop targets appear in the semantics tree with pick-up and drop-on actions, so screen-reader and keyboard users get the same operations: pick up, arrow between targets, Enter to drop, Esc to cancel.

**Automation and tests.** The protocol's drag action drives real sessions, headless included. Tests can inject an external data offer to simulate a drop from another app and read what a drag out would offer, so OS drops are covered without an OS. Sessions run on the injectable clock, and the hook API gets drag start, enter, leave, drop and cancel events.

**Security.** Data from other apps is untrusted: size caps per representation, fuzzed decoders, validated file URIs, and no promised data read before the drop.

## Deferred targets

The first release is desktop only: Linux (Wayland and X11), Windows and macOS. Web and mobile are deferred, and the architecture keeps both possible:

- **Single-threaded scheduler** stays for headless runs and debugging; a future web build without COOP/COEP headers would also need it.
- **Handle-based command buffer** with no pointer assumptions, portable to WASM.
- **Standard webgpu.h only,** so a browser WebGPU backend remains a drop-in.
- **Touch-first input model** with no desktop-only assumptions in the core.

Deferring the web removes WASM builds, browser popup windows, the ARIA DOM bridge, browser IME integration and bundled-font fallback from the first release.

## Automation, headless and MCP

One protocol serves three consumers: devtools, the test runner and AI agents. It is JSON-RPC over a pluggable transport (WebSocket, stdio, in-process) and is built on the semantics tree.

| Capability | Purpose |
| --- | --- |
| Query | Find nodes by role, label, text or test ID |
| Read | Properties, layout rects, styles |
| Act | Tap, type, scroll, drag, keys, gestures with paths |
| Screenshot | Whole surface or a single node |
| Wait for idle | Returns when no animations, frames, JS tasks or decodes are pending |
| Clock control | Pause, step, time scale |
| Subscribe | Navigation, logs, errors |

**MCP** is a thin adapter exposing tools such as query, tap, type, screenshot and wait\_for\_idle.

**Headless surfaces** come in two flavors: logic-only (layout and semantics, no rendering) for agents and most tests, and offscreen rendering for screenshots and golden tests. CI without GPUs uses a software driver (lavapipe, WARP, SwiftShader).

**Security:** disabled in release builds by default, local-only binding, auth token when enabled.

**AI and developer friendliness** also means fully typed APIs with doc comments, actionable errors, a CLI (create, dev, build, test, --headless), structured logs and agent-readable docs.

## Devtools hooks and HMR

**Hook API.** A C registry of tracepoints and callbacks that devtools, profilers and packages subscribe to. Event categories: frame begin and end per surface with phase timings, node lifecycle, signal graph changes, layout passes with dirty roots, animation start and stop, gesture arena decisions, GPU timings, resource loads, navigation transitions. Hooks are zero-cost when unused (compile-time removable, a cheap runtime check when compiled in) and versioned as a public API. A Tracy or Perfetto exporter is the first consumer.

**Devtools** built on the hooks and the automation protocol: tree inspector, flex and grid overlay, animation timeline with slow motion, frame-time graph.

**HMR.** A dev server watches files, transpiles with esbuild or SWC (tsc type-checks separately) and pushes changed modules over WebSocket. A custom module loader in QuickJS-ng swaps module records; signal state is preserved by component identity, and only changed components re-run setup. Shaders, fonts, images and styles hot-reload too. A broken module shows an error overlay while the last valid scene keeps rendering.

## Packages and plugins

The distribution unit is a package, modeled on Flutter's federated plugins. One package can contain:

- **Components:** language-level widgets, such as TSX components for TypeScript.
- **Services:** native platform APIs exposed to TS (notifications, file access).
- **Render extensions:** custom WebGPU passes in the frame graph (video, 3D viewports).
- **Devtools extensions** built on the hook API.

```
my_package/
  manifest          # name, version, ABI version, platforms, capabilities
  ts/               # public TS API
  native/
    common/         # shared C code
    windows/  linux/  macos/  android/  ios/
  idl/              # interfaces -> generated TS and C bindings
```

- **Bindings** are always generated from the IDL; no hand-written FFI glue.
- **Stable, versioned C ABI** with capability negotiation at load time.
- **Static linking by default** (required on iOS); dynamic loading is optional on desktop.
- **Federation:** third parties can add an implementation for a platform the author did not support.
- **Build-time checks:** a missing platform implementation fails the build with a clear message, not at runtime.

**Bindings for package authors.** What an author handles depends on what the package contains:

| Package content | Author writes | Works from |
| --- | --- | --- |
| Native services, render extensions, native widgets | C plus IDL; bindings for every language are generated | All languages |
| Language-level components and wrappers | Code in one language (for example TSX) | That language only |

Native interfaces follow the IDL rules: IDL types only, async results as request IDs, UTF-8 text offsets, explicit handle ownership. CI generates and compiles bindings for every supported language, so a package that breaks one language fails early. The manifest declares which languages its language-level parts support.

To limit an ecosystem split per language, the framework's standard widgets (buttons, text fields, lists, navigation) are implemented natively in C, so every language gets the same set. Authors who want cross-language reach follow the same pattern: logic in C behind the IDL, plus thin idiomatic wrappers per language.

## Dependency policy and build

The framework owns everything that defines its behavior and depends on external code only where reimplementing would be a liability.

| Dependency | Role | License |
| --- | --- | --- |
| HarfBuzz | Text shaping, font outlines | MIT |
| wgpu-native | WebGPU on native | MIT / Apache-2.0 |
| QuickJS-ng | JS engine | MIT |
| AccessKit | Desktop accessibility bridges | MIT / Apache-2.0 |
| ThorVG | SVG and Lottie | MIT |
| libwayland, xkbcommon | Wayland platform layer | MIT |
| libspng or libpng, libjpeg-turbo, libwebp, libavif, dav1d | Image codecs | Permissive (verify each) |

Rules:

- **Vendor and pin** every dependency in-tree; Rust crates via cargo vendor.
- **No third-party types in public headers.**
- **One internal module per dependency,** so replacing it touches one directory.
- **Permissive licenses only** (MIT, BSD, Apache-2.0, zlib, OFL), to keep dual licensing possible.
- **Fuzz every boundary** where external data enters: fonts, images, SVG, Lottie, IME input.

**Build.** CMake is the main build system; Corrosion drives Cargo for the Rust crates. HarfBuzz builds with CMake; ThorVG gets a project-owned CMake file. The code base is C23, compiled with Clang on every platform (clang-cl on Windows) rather than MSVC, whose C23 support is partial. CI runs ASan and UBSan builds.

## Licensing

The framework is dual-licensed: free under an open-source license, with a paid commercial license for closed-source use. The open-source license is still open.

| Option | Effect | Trade-off |
| --- | --- | --- |
| GPLv3 + commercial | Closed-source desktop and mobile apps must buy a license | Well understood; does not cover server-side use |
| AGPLv3 + commercial | Also covers server-side use, such as headless rendering or agents on servers | Deters more companies and contributors |
| Source-available, revenue threshold | Small companies use it free | Not OSI open source |

Required regardless of choice: a Contributor License Agreement before the first external contribution, and permissive-only dependencies. Final license text and the CLA should be reviewed by a software-licensing lawyer.

## Performance budgets and API stability

Budgets are enforced in CI with benchmarks from the first milestone. Targets marked open are set after the vertical slice gives baseline numbers.

| Metric | Target |
| --- | --- |
| Core + render frame time | Within 8.3 ms at 120 Hz, 6.9 ms at 144 Hz |
| Input-to-photon latency | Open |
| Cold start to first frame | Open |
| Idle memory | Open |
| Native binary size | Open |

**Frame and latency requirements.** Each window runs at its current monitor's refresh rate and retargets when moved to another monitor. Vsync is on by default; a surface may opt into immediate presentation where the platform allows it. Input latency is minimized: input is sampled as late as possible before each frame, at most one frame is queued (DXGI frame-latency waitable, mailbox presentation where available, Wayland presentation feedback), coalesced and predicted events are used, and the cursor stays a hardware cursor.

**Raw input** (unaccelerated, full-rate device data: Raw Input on Windows, relative-pointer and pointer-constraints on Wayland, unaccelerated deltas on macOS) is a per-surface opt-in for drawing, games and pointer lock. Regular UI keeps OS-accelerated pointer events, which users expect.

**Public surfaces with a versioning policy:** the TS API, the C package ABI, the automation protocol and the hook API. Each follows semantic versioning, with deprecation periods documented before breaking changes. The C API's source compatibility rules (structs only grow at their end; callers use designated initializers) are in [c-api.md](c-api.md).

## Roadmap and vertical slice

&#91;embedded content: roadmap · slice, gate, 9 phases\]

The vertical slice is a thin cut through phases 1 to 4 on Wayland: one window, all three threads plus the single-threaded scheduler, the command buffer, signals, a styled rect, one paragraph, an iOS-physics scroll view, a spring, and one automation command driving it headlessly. The slice also runs the language benchmarks and a minimal second binding. Breadth starts only after it passes the gate. The semantics tree starts in phase 3 and IME in phase 5, never later.

## Open questions

- [ ] Open-source license: GPLv3, AGPLv3 or source-available with a revenue threshold?
- [x] Decided: C23, built with Clang on every platform (clang-cl on Windows).
- [x] Decided: mobile accessibility is a requirement. AccessKit has an Android adapter and a basic iOS adapter; gaps get own bridge code.
- [ ] Numeric targets for input latency, startup, memory and size, set after the slice. Frame and latency requirements are decided (see Performance).
- [x] Decided: pushes may interrupt interactive pops (see Routing).
- [x] Decided: X11 is supported from the first release, without layer-shell.
- [x] Decided: the language benchmarks' thresholds were set in milestone 3b before they ran; results and their reading are in [ui.md](ui.md), "Benchmarks" (TypeScript on QuickJS-ng stays).
