# Axe programmable GPU layer: design

Nov 4, 2026 · @Desarrollo 5

Axe can host GPU-heavy creative apps of many kinds (drawing, 3D modeling, texture painting, compositing, data visualization) by adding three things: a WGSL superset compiled off the render thread, one resource handle model, and a retained, dirty-driven pass graph. The design was checked against commit 98446e9 and the vendored wgpu-native v29.0.1.1. This is a plan: no code, commits or dependencies were added.

## Decisions for you

Five choices change what happens next. Each has a recommendation, and section 4 and section 5 hold the detail.

| # | Decision | Recommendation | Why |
| --- | --- | --- | --- |
| 1 | Run the first GPU slice, M5a, before M3c? | Yes | M5a tests the riskiest idea and gives M3c a real always-changing node to test damage against |
| 2 | Relax "all WebGPU calls on the render actor"? | Yes, narrowly: one GPU admin thread creates objects; encoding and submission stay put | wgpu-native has no async pipeline creation, so a driver compile would otherwise stall every frame |
| 3 | Add a persistent per-surface back store in M3c? | Yes | Damage tracking, backdrop reads, and window capture all need retained pixels, which a swapchain does not give |
| 4 | Approve the dependencies in section 5? | Decide at M5b and M5e, not now | None is needed before M5b |
| 5 | Which reference proof comes first in M5g? | Drawing, then modeling, then 3D painting | That follows the dependencies, and the first needs no geometry. Your product priority can override it |

Five findings from the vendored code shaped the design:

- `wgpuDeviceCreateRenderPipelineAsync` and the compute version are `unimplemented!()`, so calling them panics.
- `wgpuShaderModuleGetCompilationInfo` is also `unimplemented!()`, so structured diagnostics are not available. Error text must come from error scopes.
- Error scopes are one device-wide stack, not per thread. Two threads using them can pop each other's scope.
- `Set*Label` functions panic too, so labels must go in descriptors.
- `wgpuDevicePopErrorScope` unwraps an empty stack, so an unbalanced pop aborts the process.

## Workloads

Axe's GPU layer serves a class of apps, not one app, and no two of them need the same things. A drawing app needs no meshes, a modeler needs no tiles, and a 3D painter needs both. So the core gets general primitives, and each workload is a test of them.

| Workload | Examples of the class | Hardest need |
| --- | --- | --- |
| Drawing | Raster and vector illustration, on huge zoomable canvases | Pointer-to-pixel latency on sparse layers |
| Compositing | Image editors, video and motion tools, node-based filters | Partial evaluation, long filters, exporting at set times |
| 3D modeling | Mesh editors, sculpting, CAD | Fast partial mesh edits, element picking, overlays |
| 3D painting | Texture painting on meshes | All of the above at once |
| Visualization | Plots, maps, point clouds, medical and scientific volumes | Millions of primitives, and data that is not a mesh |

| Capability | Drawing | Compositing | Modeling | 3D painting | Visualization | Milestone |
| --- | --- | --- | --- | --- | --- | --- |
| Canvas and effect shaders | Some | Core | Some | Some | Core | M5a, M5b |
| Compute and the graph | Core | Core | Some | Core | Core | M5c |
| Sparse tiles and regions | Core | Core | No | Core | No | M5c |
| Frequent partial buffer writes | No | No | Core | Some | Core | M5c |
| Budgeted background work | Some | Core | Some | Some | Core | M5d |
| Geometry: meshes, lines, points, depth | No | No | Core | Core | Core | M5e |
| Pointer streams | Core | No | Some | Core | No | M5f |
| Element picking and region readback | Some | Some | Core | Core | Some | M5f |

**The two-workload rule keeps any one app from shaping the core.** A feature enters Axe only if at least two workloads need it, and only if apps could not build it as a shader plus a graph pass. Everything else belongs in the app or in an optional package.

The section Test workloads walks each class through the design. 3D painting gets the longest walk because it combines the others, not because it is the target.

## 1. Architecture summary

The GPU layer sits beside the display list, not inside it: the list stays renderer-agnostic and only names handles. Logic declares shaders, resources and graphs once as data. After that, the core and the render actor run them without JS on the frame path.

Five parts are new:

- **AXSL**, the shading language: WGSL function bodies under a small declarative header. It compiles to WGSL in C, on the existing job pool.
- **The GPU admin thread**, the only new thread. It creates shader modules, pipelines and large resources, and it is the only user of error scopes.
- **The resource table**, one generational handle kind for textures, buffers, meshes, programs and graphs.
- **The work graph**, a retained list of passes with triggers, dirty regions and a budget class. wgpu already inserts barriers, so this is a scheduler, not a Frostbite-style frame graph.
- **Blobs**, a side channel for large bytes. Ops stay small; a mesh or texture travels as a detached buffer that an op names by ID.

&#91;embedded content: architecture · threads and what crosses between them\]

The mailbox may replace a list, so GPU ops take the FIFO path, and each list names the sequence number it needs.

### Key decisions

| Area | Decision | Rejected | Why rejected |
| --- | --- | --- | --- |
| Language | WGSL superset: new top-level declarations, WGSL bodies copied verbatim | A new small language | Needs a second type checker that must track naga and Tint, including overloads and abstract numerics |
| Language | Same | Slang, GLSL through naga, WESL | Slang is a large C++ dependency; GLSL input is not in the browser; WESL has no C implementation and no shader types |
| Compiler | C library in `src/shader/`, run on the job pool, plus an `axe-shaderc` build tool | Compile in JS or Rust | Must run from C apps at runtime; a C target fits the existing fuzz setup |
| Validation | Both: AXSL rules in our compiler, WGSL rules in naga through the device | One only | Our compiler cannot check WGSL types; naga cannot check AXSL rules |
| Object creation | One GPU admin thread | Creating on the render actor | A driver compile on that thread stalls every surface |
| Object creation | Same | Standard async creation | `unimplemented!()` in wgpu-native v29; kept as the browser path behind a capability |
| Handles | One `UI_HANDLE_RESOURCE` kind, allocated by logic like nodes | One handle kind per resource type | Only 16 kinds exist (4 bits), and lifetime, budget and pinning are the same for all |
| GPU work | Retained graph with triggers | JS encoding passes per frame | Puts JS on the frame path and adds a logic round trip to every pointer sample |
| GPU work | Same | A worklet running JS on the render thread | Breaks the rule that the frame path never waits on JS |
| Long work | Two budget classes, time-sliced on one queue | A separate compute queue | WebGPU exposes one queue |
| Showing results | Texture nodes plus a `gpu_seq` on each display list, sent through the FIFO inbox | Carrying work inside the display list | The mailbox may replace a list, which would lose the work |
| Partial updates | `@reach` metadata and region lists in the graph | Re-evaluating whole textures | Large layered textures do not fit (section Resources) |
| 3D | Primitives only: meshes, lines, points, depth, MSAA, draws, `axe:pbr` | A scene graph, materials and cameras in Axe | Turns Axe into an engine; apps can build them from the primitives |
| Pointer input | A core-owned sample stream written straight into a GPU buffer | Each sample sent through JS | Adds a logic round trip at the pointer's rate |
| CPU renderer | Each shader item carries a fallback color | Interpreting WGSL on the CPU | Large, and lavapipe already covers software execution |
| Native access | Internal extension seam for ThorVG; public ABI deferred | A public device API now | Freezes webgpu.h types into a stable ABI before any package needs it |

## What the code does today

Yes, something can animate forever without JS: the frame loop already runs on `root->animating` alone. Only springs set that flag today, so a continuous shader needs one new source of frame demand, not a new loop.

### How a frame is requested

1. The drawn frame's present asks for the next callback, in `ui_wl_pre_present`.
2. The compositor's callback reaches `on_frame` (`core.c`), which sets `frame_ready` and clears `awaiting_frame`.
3. `frame()` checks `head_ready` and `surface_has_work`: forced draw, resize, new configure, `root->dirty` or `root->animating`.
4. With work, `commit()` builds and publishes a list, and the loop continues from step 1.
5. Without work, the surface goes idle and asks for no callback. `wake_idle` calls `surface_request_frame` once when work next appears.

### What this means for the design

| Finding | Where | Consequence |
| --- | --- | --- |
| `root->animating` is rebuilt each frame by scanning every node | `ui_scene_animate` | Frame demand must become a set of reasons with an active list; M3c should do this anyway |
| A paused clock keeps a spring active, so a window keeps drawing identical frames | `surface_has_work`, `ui_scene_animate` | A continuous node counts as work only if animation time moved since its last draw |
| `ui_core_idle` ignores `animating` | `core.c` | `ui_app_frame` still returns with a continuous node present. The automation protocol's wait-for-idle must define the same rule |
| Commands apply only inside `frame()`, which returns early if no surface is ready | `frame()` | With no frame callbacks (a locked KDE session, for example) queued GPU ops never apply. M5d needs a fallback tick |
| The render actor waits on its signal with no timeout | `actor_thread` | It cannot poll for GPU completion between frames. It needs a timed wait while work is in flight |
| `wgpuSurfaceGetCurrentTexture` runs on the same thread as everything else | `draw_window` | A blocking acquire stalls background slices, so slices run after present, not before acquire |
| Atlas updates go through the FIFO inbox, and each list names the `atlas_seq` it needs | `render_step`, `commit` | The exact pattern for GPU work: FIFO for ops, a sequence number on the list |
| Text preparation already holds a buffer back until it is ready, and `ui_app_frame` waits for it | `wait_prepared` | The same mechanism makes shader compiles byte-identical under both schedulers |
| Each surface pass starts with `LoadOp_Clear` | `encode` | The swapchain holds no retained pixels, so damage tracking needs a back store |
| The maximum op is 65,532 bytes and a buffer is 64 MiB | `wire.h` | Meshes and textures cannot travel as ops. `ui_js_array_buffer_detach` already gives zero-copy transfer |
| The device requests no optional features and default limits | `init_device` | Capabilities must be requested at startup, before the app knows what it needs |
| `capture` blocks in a poll loop and refuses windows | `capture` | Readback needs a non-blocking path; the back store removes the window limit |

### A bug to fix first

Real device loss looks silently ignored today. wgpu-native reports it through `ErrorSinkRaw::handle_error` with the reason `Destroyed`, and that is the only reason this path sends. Axe's `on_device_lost` returns early on `Destroyed`, so `device_lost` is never set. Heavy shaders make device loss likely, so M5a fixes this by telling our own `wgpuDeviceDestroy` apart from a real loss.

## 2. Language: AXSL

AXSL is a constrained WGSL superset: a declarative header in the spirit of gdshader, with function bodies that are plain WGSL, copied verbatim. A file is `.axsl`. There is no second type system, so the compiler stays small, and anything learned here is real WGSL.

What AXSL removes from raw WGSL is the plumbing: bind groups, entry-point structs, stage inputs and outputs, and render-target declarations. WGSL function bodies are already compact (`vec4f`, `let`, swizzles), so they gain little from new syntax.

### Syntax

Only these top-level forms are new; everything else is WGSL.

| Form | Meaning |
| --- | --- |
| `shader_type canvas;` | Required first. Selects the built-ins and entry points |
| `render_mode blend_add, cull_none, topology_lines;` | Fixed-function state, in the shader as in gdshader |
| `uniform name: type = default @hints;` | A parameter, numeric or sampled texture, reflected to tools |
| `image name: rgba16float;` | A storage texture, write-only unless marked `@read_write` |
| `storage name: array<Point>;` | A storage buffer, read-only unless marked `@read_write` |
| `varying name: vec3f;` | A vertex-to-fragment value, with locations assigned for you |
| `include "axe:pbr";` | Reuse. Each file is included once |
| `requires float32_filterable;` | A capability. Missing ones give a compile error naming the feature |
| `fn vertex()`, `fn fragment()`, `fn compute()` | Stage entry points, with no parameters and no return value |
| `attribute name: f32;` | A custom per-vertex or per-instance input, matched to a mesh layout by name |

Samplers are predeclared, not declared: `LINEAR_CLAMP`, `LINEAR_REPEAT`, `NEAREST_CLAMP`, `TRILINEAR_REPEAT`, `ANISO_REPEAT`. So `textureSample(albedo, LINEAR_REPEAT, uv)` stays valid WGSL, and no call needs rewriting. Only samplers a shader uses get bindings.

### How it compiles

The compiler needs no expression grammar. It tokenizes the file, parses top-level declarations, and finds function extents.

- **Built-ins and numeric uniforms** become `var<private>` globals, in the style of Godot's writable `COLOR`. Textures, images and storage buffers stay direct bindings, since WGSL cannot copy them.
- **A generated entry point** fills the inputs, calls your function, and returns the outputs.
- **Only names the file uses** are emitted, found by a token scan.
- **Function bodies are untouched**, so lines and columns inside them survive exactly.

Drivers should remove the private copies after inlining, and M5a measures that. If they cost more than 2% on the reference shaders, uniforms switch to token rewriting, which needs scope tracking. Assigning to a uniform is a compile error, found by token pattern.

### Shader types

| Type | Runs | Reads | Writes |
| --- | --- | --- | --- |
| `canvas` | A node's rect, inside the UI pass, with no pass break. Also a graph pass over a target region | `UV`, `PIXEL`, `SIZE`, `TIME`, `FRAME`, `NODE_COLOR`, `OPACITY` | `COLOR`, linear and premultiplied. Axe applies the node's rounded-rect coverage, so `corner_radius` still works |
| `effect` | A node's rect, over what is under it or over its subtree | The canvas inputs, plus `BACKDROP` or `CONTENT` | `COLOR`. Declares `@reach(px)` for its read radius |
| `spatial` | Geometry in a graph render pass, from a mesh or from a storage buffer | `POSITION`, `NORMAL`, `TANGENT`, `UV0`, `UV1`, VERTEX\_INDEX, INSTANCE\_INDEX, `MODEL`, `VIEW`, `PROJ`, `VIEW_PROJ`, `CAMERA_POS`, `TIME` | `CLIP`, `COLOR`, optional extra targets. `vertex()` is optional |
| `compute` | A graph dispatch over a region | `PIXEL`, `REGION`, `INVOCATION`, `TIME`, `FRAME` | Images and storage buffers |
| `library` | Nowhere; include only | Nothing | Functions, structs and constants |
| `raw` | Whatever its WGSL declares | Reflected by parsing `@group`/`@binding` declarations | The escape hatch: plain WGSL, bindings matched by name |

`TIME` is `dl->anim_time_ns` in seconds, wrapped at 3,600 s to keep float precision, so it obeys the paused and stepped clock. Godot wraps the same way.

A `spatial` shader does not need a mesh. With `VERTEX_INDEX` and a `storage` buffer it can draw plots, point clouds and overlays directly, and `attribute` carries any custom per-vertex data, so Axe assumes no fixed vertex format.

### Example: canvas

```wgsl
shader_type canvas;

uniform tint: vec4f = vec4f(0.2, 0.5, 1.0, 1.0) @color @label("Tint");
uniform speed: f32 = 1.0 @range(0.0, 4.0, 0.01);

fn fragment() {
    let wave = 0.5 + 0.5 * sin(UV.x * 12.0 + TIME * speed);
    COLOR = mix(NODE_COLOR, tint, wave) * OPACITY;
}
```

### Example: spatial

```wgsl
shader_type spatial;
include "axe:pbr";

uniform base_color: texture_2d<f32> @srgb @default(white);
uniform roughness: f32 = 0.5 @range(0.0, 1.0);

varying world_pos: vec3f;

fn vertex() {
    world_pos = (MODEL * vec4f(POSITION, 1.0)).xyz;
    CLIP = VIEW_PROJ * vec4f(world_pos, 1.0);
}

fn fragment() {
    let albedo = textureSample(base_color, ANISO_REPEAT, UV0).rgb;
    let n = normalize(NORMAL);
    COLOR = vec4f(pbr_shade(albedo, roughness, n, world_pos, CAMERA_POS), 1.0);
}
```

### Example: compute

```wgsl
shader_type compute;

uniform src: texture_2d<f32>;
image dst: rgba16float;
uniform amount: f32 = 1.0 @range(0.0, 4.0, 0.01) @label("Sharpen");

const RADIUS = 4;

@workgroup(8, 8) @reach(4)
fn compute() {
    var sum = vec4f(0.0);
    for (var y = -RADIUS; y <= RADIUS; y++) {
        for (var x = -RADIUS; x <= RADIUS; x++) {
            sum += textureLoad(src, PIXEL + vec2i(x, y), 0);
        }
    }
    let c = textureLoad(src, PIXEL, 0);
    textureStore(dst, PIXEL, c + (c - sum / 81.0) * amount);
}
```

### Compute declarations

The graph binds `src` and `dst` by name, and nothing in the shader names a group or binding. Buffers work the same way: `storage points: array<Point>;` is all a simulation or an aggregation pass declares.

- **Workgroup size** comes from `@workgroup(x, y, z)`, default 8 × 8 × 1.
- **Regions are exact.** The generated entry skips invocations outside `REGION`, so the graph can dispatch any rectangle without the shader knowing.
- **`PIXEL` is the position in the target,** which keeps a shader correct when the scheduler splits its region.
- **`@reach(r)` declares how far a shader reads from its input.** It is the key to partial evaluation: dirty regions grow by `r`, and a shader without it cannot be sliced or limited to tiles.
- **`var<workgroup>` and atomics** stay plain WGSL.

Baseline WebGPU allows `read_write` storage only on `r32*` formats. So a pass reads through `uniform` and writes through `image`, as above. `@read_write` on another format needs a capability, and the compiler says so.

### Metadata

Each compile produces the WGSL, a source map and a `ui_shader_info` reflection record. No WebGPU types appear in it, so it can cross the public C API.

| Metadata | Used by |
| --- | --- |
| Shader type, entry points, render modes | Pipeline creation |
| Uniform block: name, offset, size, type, default | `set_param`, springs, generated UI |
| UI hints: `@range`, `@color`, `@enum`, `@label`, `@section`, `@unit`, `@tooltip` | Generated inspector panels |
| Bindings: name, kind, format, access, sampler use | Graph validation and bind-group layouts |
| Workgroup size and `@reach` | The scheduler |
| Vertex attributes used, varyings, target outputs | Mesh and pipeline matching |
| Required capabilities | Compile errors and the capability event |
| Files included, with content hashes | Hot reload and the pipeline disk cache |

### Includes

There are no macros and no preprocessor. `include` pastes a `library` file once, at top level.

- **Names:** `axe:` files are embedded in the binary. Other names go through a resolver the host supplies.
- **Runtime shaders:** a user-authored shader gets an in-memory map of names to sources, never filesystem access. A shader cannot read a file by including it.
- **Variants:** use WGSL `override` constants and `const`, which need no preprocessor.

### Where the compiler runs

The compiler is a pure C library in `src/shader/`, with no GPU and no global state. Three callers share it:

1. **Runtime:** a job on the existing pool. With zero workers it runs inline, as text preparation does.
2. **Build time:** `axe-shaderc` embeds compiled WGSL and reflection in release bundles, as `axe-jsc` does for bytecode.
3. **Fuzzing:** `fuzz_shader` calls it directly.

It treats its input as hostile, with hard limits: 1 MiB of source, 16 include levels, 64 files, 64 nesting levels, 256 uniforms. Parsing is iterative and linear in input size, and memory comes from one capped arena.

The 64-level cap sits below naga's own limits (recursion 256, braces 127), so input that would reach them is refused first.

### Errors

A failed compile becomes a `shader_compiled` event with a list of diagnostics, each with file, line, column, severity and message. It never reaches `on_uncaptured_error`.

1. Our compiler reports declaration errors directly, with exact positions.
2. The admin thread wraps `wgpuDeviceCreateShaderModule` in a validation error scope.
3. naga's message arrives as text only, so we parse its `┌─ name:line:col` lines.
4. The source map turns generated lines into file and line. Columns are unchanged, because bodies are verbatim.
5. A message that does not parse still arrives, whole and without a position.

The text format is a wgpu implementation detail. A corpus test pins it, so a wgpu upgrade that changes it fails in CI. If it proves brittle, direct use of naga through a small Rust shim is the fallback. That shim is listed under Risks because it may mean linking two Rust static libraries.

### Validation

Validation happens in both places, and each layer checks only what it can.

| Layer | Checks | When |
| --- | --- | --- |
| Our compiler | AXSL syntax, declarations, hints, reserved names, limits, capabilities, uniform writes | On a worker; no GPU needed, so it also runs in `axe-shaderc` and in editors |
| naga, through the device | WGSL types, control flow, resource use | On the admin thread |
| Pipeline creation | Layout and target compatibility | On the admin thread. Failure here is an Axe bug, reported as an event |

### Hot reload

A program handle is stable, and its contents are versioned. Saving a file never disturbs what is on screen until the new version is proven good.

1. The in-process watcher from M3b.3 sees a change to the file or to anything it includes.
2. A worker compiles it; the admin thread validates it and builds pipelines.
3. On success, the new pipelines swap in through the FIFO inbox at a frame boundary. Graph passes using the program are marked dirty.
4. On failure, the last good version keeps drawing. The diagnostics go to logic and to the dev overlay.
5. Uniform values carry over by name and type. New uniforms take their defaults, and removed ones are dropped.

If a program has never compiled, its node draws its fallback color, which is the node's background. Release bundles drop the watcher, but the runtime compile API stays, because user-authored shaders need it.

## Resources

Everything the GPU reads or writes is one kind of thing: a resource named by a generational handle. Images and ThorVG output are textures in that table, so a node, a shader and a graph pass all refer to them the same way.

### Handle model

A new handle kind, `UI_HANDLE_RESOURCE`, is allocated by logic and mirrored by the core, as nodes are. So creating a resource needs no round trip, and a stale handle is detected, not dereferenced.

| Type | Source | Notes |
| --- | --- | --- |
| `texture` | Empty, file, blob, or vector | Decoding runs on the job pool. 2D, array, cube or 3D, with mips |
| `buffer` | Empty or blob | Uniform, storage, vertex, index or indirect use, in any combination core WebGPU allows |
| `mesh` | File or blob | Buffers with any vertex layout, plus submeshes. Compute can write them |
| `program` | Source text, or a compiled bundle entry | One AXSL shader with its pipelines |
| `graph` | Graph description | Section GPU work scheduling |

A `vector` texture holds an SVG or Lottie source, a size and a scale. ThorVG draws it on the render actor through the internal extension seam, and the core sets its playhead, as the architecture already requires.

### Large data: blobs

Ops stay small. A blob is bytes that move once, by ownership.

- **C:** `ui_blob_send(id, data, size, free_fn)`.
- **TypeScript:** `gpu.upload(bytes)` detaches the ArrayBuffer through `ui_js_array_buffer_detach`.
- **Order:** blobs travel on the `from_logic` queue, so an op can name one sent before it.
- **Portability:** the wire carries only an ID, which keeps the no-pointers rule for a future WASM build.

File paths are the normal route for assets, as `load_font` is for fonts. Blobs are for generated data and for shader source.

Frequent small edits take a third route: `write_resource` over a byte range, with several writes to one buffer merged per frame. A mesh editor changing 10,000 vertices of a 1 M vertex mesh pays for 10,000.

### Lifetime

Ownership is explicit, as the architecture requires: `destroy_resource` releases a handle, and finalizers are only a safety net.

1. Destroying a handle invalidates it at once for logic and for later commands.
2. The render side keeps the object until the last display list or graph run that used it has finished.
3. That is the rule `protect_epoch` already applies to glyphs, generalized to resources.

Each resource carries a version that increments on every write. Versions drive dirty propagation in the graph and damage in M3c.

### Budgets

Axe does its own accounting, because WebGPU exposes no memory query. `wgpuGenerateReport` counts objects, not bytes.

- **Accounting:** bytes are computed from each descriptor, by class: images, render targets, app textures, buffers, meshes.
- **Budget:** a `gpu_budget_bytes` field in the app description, with a default of 2 GiB (an initial value, to tune in M5d).
- **Over budget:** creation fails with an event, never silently. A `gpu_status` event warns at 80%.
- **Real out-of-memory:** the admin thread wraps large allocations in an `OutOfMemory` error scope, and failure becomes the resource's result.
- **Eviction:** Axe evicts only what it can rebuild, such as the decoded-image cache and idle layer textures. App data is never evicted behind the app's back.

### Very large textures

Whole-texture layer stacks at 8K cannot fit, so any layered editor needs sparse tiles: drawing, image editing and 3D painting alike. The GTX 1650 has 4 GiB.

| Texture | Size | With mips |
| --- | --- | --- |
| 8K RGBA8 | 256 MiB | 341 MiB |
| 8K RGBA16F | 512 MiB | 683 MiB |
| 8K RGBA32F | 1 GiB | 1.33 GiB |
| 20 layers × 6 channels, 8K RGBA16F | 60 GiB | 80 GiB |
| One 256 × 256 RGBA16F tile | 512 KiB | n/a |

A fully covered 8K texture is 1,024 tiles of 256 pixels. Most layers cover a small part of it, so sparse storage pays.

WebGPU has no sparse textures, so tiles live in pools of array textures. A page table in a buffer maps tiles to slots. Axe provides array textures, region copies and indirect work. Tile allocation belongs to the app or to an optional `@axe/tiles` package.

Axe adds two things for large textures:

- **Budgeted uploads.** A `write_resource` larger than 16 MiB is cut into per-frame chunks, because `wgpuQueueWriteTexture` copies on the calling thread.
- **A built-in `mips` pass,** limited to a region. WebGPU has no mipmap generation, and any texture edited at full size and shown smaller needs it. Without region support, each edit would rebuild whole mip chains.

### Formats

`rgba16float` is the working format for editing and HDR: it is core, renderable, storable, filterable and blendable.

| Format | Use | Requirement |
| --- | --- | --- |
| `rgba8unorm-srgb` | UI, imported color | Core. Not storable, so compute writes to it are impossible |
| `rgba8unorm` | Masks, display copies | Core, storable |
| `rgba16float` | Layers, HDR viewport, HDR back store | Core |
| `rg11b10ufloat` | Cheap HDR viewport target | `rg11b10ufloat-renderable` |
| `r32float` | Heights, masks. `read_write` storage | Core; filtering needs `float32-filterable`, blending needs `float32-blendable` |
| `rgba32float` | High-precision working data | Core, unfilterable without `float32-filterable` |
| `r32uint` | Object, vertex, edge and face IDs | Core |
| `depth32float` | Viewport depth | Core |
| `rgba16unorm` | True 16-bit color | wgpu-native extension, not in browsers |
| BC formats | Imported read-only textures | `texture-compression-bc`; never edit targets |

`rgba16float` has a real weakness: its 10-bit mantissa gives about 1/1,024 steps between 0.5 and 1.0. A true 16-bit pipeline would need `rgba16unorm` or `rgba32float`. This is listed as an open question.

### Capabilities

Features must be requested when the device is created, so `init_device` requests a fixed allowlist of everything the adapter offers. Logic then receives a `gpu_caps` event with the features and limits.

- **Standard features:** `timestamp-query`, `float32-filterable`, `float32-blendable`, `rg11b10ufloat-renderable`, `bgra8unorm-storage`, `texture-compression-bc`.
- **Native, behind `UI_WEBGPU_WGPU_NATIVE`:** adapter-specific format features, 16-bit norm formats and the pipeline cache.
- **Not usable yet:** `webgpu.h` defines `TextureFormatsTier1` and `Tier2`, but wgpu-native v29's `conv.rs` does not map them.
- **Limits:** request the adapter's values for texture size, array layers, buffer size, storage bindings and storage textures.

Limits differ widely here: lavapipe reports 16,384 maximum texture size and a 128 MiB storage range, and the GTX 1650 reports 32,768 and 4 GiB. Tests must run within the lavapipe values.

### Readback

Readback is an op answered by an event, and it never blocks the render actor. The existing `capture` stays, for tests only.

1. `read_resource` names a texture region or a buffer range, and a request ID.
2. The render actor copies into a pooled staging buffer and calls `wgpuBufferMapAsync`.
3. `render_step` polls without waiting. Completion copies the bytes to a blob.
4. A `readback_done` event delivers the blob to logic.

| Use | Reads | Notes |
| --- | --- | --- |
| Color picker | 1 × 1 of any texture, including the back store | Common to every workload |
| Object or element pick | 1 × 1 of an `r32uint` ID target | The ID pass renders on demand |
| Box select, lasso | A rectangle of IDs | Modelers and editors |
| Compute results | A buffer range | Vertices, histograms, statistics |
| Export | A whole texture | Split into chunks, in the background class |

A small pool of preallocated staging buffers serves single-texel reads, and at most 8 reads run at once. Expect two to three frames of latency, and M5f measures it.

## GPU work scheduling

A pass graph described as data is the right model, and JS stays off the frame path. A static graph that runs every frame is not enough for interactive editors, though, so the design adds triggers, regions and streams.

### Three changes to the graph you expected

1. **Most graphs should not run every frame.** Each pass has a trigger. A one-shot job is just a graph with `on_demand`, so no separate imperative API exists.
2. **Variable work needs data channels.** Dirty regions, pointer samples and edited vertices change every frame, so they flow through region lists, indirect buffers and streams. If the graph's shape changes, because a layer was added, JS redefines it. That happens on an edit, not per frame.
3. **It is a scheduler, not a classic frame graph.** wgpu inserts barriers, and WebGPU cannot alias memory, so those two usual benefits do not apply. The graph's value is ordering, dirty propagation, budgets, hot reload and determinism.

### Declaring work

```ts
const filter = gpu.graph({
  passes: [
    { name: 'blur', program: blur, bind: { src: input, dst: output },
      params: { radius }, region: 'dirty',
      when: 'inputs_changed', budget: 'background' },
    { name: 'mips', kind: 'mips', texture: output, region: 'dirty' },
  ],
});
```

| Field | Values |
| --- | --- |
| `kind` | `compute`, `render`, `copy`, `mips` |
| `when` | `every_frame`, `inputs_changed`, `on_demand`, `on_stream` |
| `budget` | `frame` (its result is needed now) or `background` (it may take many frames) |
| `region` | `full`, `dirty`, a rectangle, or a tile-list buffer |
| `bind`, `params` | Resources and uniforms by name, checked against reflection when the graph is defined |
| `targets`, `depth`, `draws` | Render passes: attachments, and draws from a mesh or a plain vertex count, with ranges and instances |
| `time` | The animation clock, or an explicit value, for exporting at set times |

A graph description is a blob, validated on a worker when it is defined. Hostile or malformed graphs are refused there, and `fuzz_graph` covers that decoder.

### Who decides what

The core decides, and the render actor executes. The core already owns the clock, the versions and input, so a graph pass needs no JS and no logic round trip.

- **The core** applies ops, tracks resource versions and marks passes dirty. Graph ops go down the FIFO inbox with a sequence number.
- **Each display list** records the `gpu_seq` it needs, like `atlas_seq`. The render actor applies inbox messages up to that number before drawing.
- **The render actor** runs passes and reports completion. A `GPU_OUTPUT` message tells the core which resource, version and rectangle changed.

Background results therefore reach the screen one frame after they finish, which is acceptable. Frame-class passes are known in advance, so their node is marked dirty in the same frame.

### A render step

1. Drain the inbox up to the list's `gpu_seq`.
2. Run the frame-class passes that are due and predicted to fit.
3. Encode the display list, submit and present.
4. Run background slices with the budget that is left.
5. Poll for completions without waiting, and sleep with a timeout while work is in flight.

Step 4 comes after present, so a slice never delays the current frame's submission.

### Keeping the UI responsive

There is one queue and GPU preemption is coarse, so the only real tool is short submissions. Three rules follow.

- **No submission of background work exceeds 2 ms.** This also keeps long work far from driver timeouts.
- **The UI never waits for frame-class work.** A pass predicted to overrun is skipped for that frame, and the list shows its last output. A 30 ms viewport cannot pull the UI down to 30 fps.
- **Background work gets what is left.** The budget is the refresh period, minus measured UI and frame-class GPU time, minus a 1.5 ms margin. At worst it still gets one slice every 4 frames, so it cannot starve.

The architecture's budgets (8.3 ms at 120 Hz, 6.9 ms at 144 Hz) cover core and render CPU time, and this design holds them. It adds a GPU budget to that table, since a CPU-only number cannot show a dropped frame caused by a compute pass.

### Slicing long work

The scheduler cuts a region into tiles, dispatches a few, and measures. Shader metadata makes this safe.

- **Measuring:** timestamp queries when available, else `wgpuQueueOnSubmittedWorkDone` timing from the host. An average of nanoseconds per pixel, per program, predicts the next slice.
- **Atomic output:** a background pass writes to a private copy, and the texture swaps when it finishes, so the UI never shows a half-done result. `progressive: true` opts out.
- **Cancellation:** if an input changes mid-run, the run stops at the next slice and restarts from the combined dirty region. Dragging a slider therefore never queues up work.
- **Unsliceable passes:** a pass with no `@reach` runs as one dispatch, and it logs a warning when it exceeds the cap.

Uploads and readbacks draw on the same budget.

### Showing the output

A node shows GPU output through a property, so it lays out, clips and animates like any other node.

| Op | Display list item | Result |
| --- | --- | --- |
| `set_node_texture` | `UI_DL_TEXTURE` | The node shows a texture, with a fit mode and sampler. A 3D viewport is this |
| `set_node_shader` | `UI_DL_SHADER` | A `canvas` shader fills the node. No pass break, only a pipeline change |
| `set_node_effect` | `UI_DL_EFFECT` | An `effect` shader reads the backdrop or the node's subtree |

Each item carries a resource handle and a fallback color, and no WebGPU types.

**Node-sized textures** matter to every viewport and canvas. A texture can declare `size_from: node`, and the core resizes it when layout changes. Without this, a viewport resize needs a JS round trip and shows a stretched frame.

### Damage tracking

GPU-driven nodes change for reasons outside the scene, so M3c's damage cannot come from node diffs alone.

| Node | What damages it | What M3c must support |
| --- | --- | --- |
| Texture reader | A new resource version, with a rectangle | External damage: an edge from resource to node, plus a sub-rect |
| Continuous `canvas` shader | Every frame in which animation time moved | Always-dirty nodes, in a list segment of their own |
| Parent of a continuous node | Nothing of its own | Layer caching must not cache across it |
| Backdrop `effect` | Anything under its rect, grown by `@reach` | Damage inflation, and a readable back store |
| Subtree `effect` | Its subtree | Paint bounds grown by `@reach` |

Section 4 turns this into a list of M3c changes.

### Is a render-thread C API still needed?

Not for any workload in this design, and not yet for anyone. The graph covers every walk in Test workloads. ThorVG does need the device, so an internal seam is required anyway: a small `ui_gpu_ext` table of `prepare`, `encode` and `release`.

The seam stays private until a real package needs it. Making it public would cost:

- **The header rule:** webgpu.h types would enter a public header, against "no third-party types in public headers".
- **ABI stability:** every wgpu-native upgrade would become an ABI event for packages.
- **Frame safety:** native code on the render thread can stall frames, and no budget can stop it.
- **Accounting:** its objects escape the resource table, error handling and device-loss recovery.
- **Portability:** it would not work with a browser backend or the CPU renderer.

Hardware video decode is the likeliest first need. If a package does need it, ship a separate opt-in header marked unstable, rather than extending the stable C API.

## 3D

Axe provides GPU primitives and a shader library, and the app provides the 3D world. The test for each feature: if an app could write it as a shader plus a graph pass, it stays out of Axe.

| Concern | In Axe | In the app |
| --- | --- | --- |
| Meshes | Mesh resource, any vertex layout, submesh ranges, byte-range updates, and instanced, indirect and mesh-less draws | Which meshes draw, and when |
| Depth | `depth32float` attachments, load and store, compare modes in `render_mode` | Pre-pass strategy |
| MSAA | Sample count on targets and resolve. WebGPU guarantees 1 and 4 samples | Choosing it |
| Cameras | The names `VIEW`, `PROJ`, `VIEW_PROJ`, `CAMERA_POS` in a fixed block that a graph fills from params, once per view | Orbit logic, matrices, and how many views |
| PBR | `axe:pbr`: GGX, Smith, Fresnel, tone mapping, image-based lighting helpers | Material model, environment maps, lights |
| Picking | Readback of texels and rectangles, `r32uint` targets, a small `axe:pick` library | The ID pass and what a pick means |
| Scene graph, transforms | Nothing | All of it |
| Culling, LOD | Nothing | All of it, in compute if wanted |
| Shadows, skinning, animation | Nothing | All of it |
| Gizmos and grids | Nothing | `spatial` shaders |
| Lines, points and overlays | Topology in `render_mode`, and `axe:lines` for wide lines, since WebGPU lines are one pixel | Which overlays to draw: wireframes, vertices, selection outlines |

Camera input takes a logic round trip at first: pointer event, JS, `set_param`, one or two frames. Later, the animated-values expression graph can bind a camera to a gesture without JS, and no design change is needed.

### Mesh and texture import

Import is a separate optional module, outside the core and fuzzed like every other boundary. The core only understands its neutral mesh format, so an app with its own loader needs none of this.

- **In scope:** glTF 2.0, OBJ, and the image formats the architecture already plans. Radiance `.hdr` for environment maps, with an own decoder.
- **Later:** FBX and OpenEXR, both common in production pipelines but heavy to support.
- **Out of scope:** USD, Alembic, scene import, and animation import.
- **Path:** `load_resource` decodes on a worker, then uploads through the budgeted path, then sends `resource_ready`.
- **Tangents:** must match the bakers' convention, so normal maps need MikkTSpace rather than an ad hoc calculation.

Section 5 lists the candidate libraries. None is added until you approve it.

## Test workloads

The design holds for every class, and walking them found ten gaps that any single reference app would have missed. All ten are now in the design. The figures below are design estimates, not measurements.

### Drawing: one stroke on a 2D canvas

A drawing app has no geometry, so it needs nothing from M5e. The canvas is sparse layers of tiles, shown through one `canvas` shader that reads the tile pool.

1. **Input.** The core appends pointer samples to a stream bound to the canvas node, with no JS.
2. **Stamps.** A compute pass expands samples into stamps along the stroke.
3. **Raster.** Stamps draw directly into a stroke mask in canvas space. There is no projection and no culling.
4. **Copy on write.** The first touch of a tile copies it to the undo pool.
5. **Layer stack.** Compute passes blend dirty tiles only.
6. **Display.** The canvas node redraws the stroke's screen rectangle.

Pan and zoom re-render no layers: they are two params on the canvas shader, so they can run on a spring at refresh rate. Mips cover zoomed-out views, and nearest sampling covers pixel-level zoom.

WebGPU has no framebuffer read, so blend modes such as overlay and soft light run in the compute layer stack rather than as fixed-function blending.

### 3D modeling: one edit

A modeler moves little data per frame but needs exact selection and dense overlays. Take a drag of 10,000 selected vertices on a 1 M vertex mesh, in four views.

1. **Select.** A click renders the ID target on demand and reads one texel. A box select reads a rectangle of IDs.
2. **Hold the model.** The app owns topology, in JS or C. Axe sees only buffers, so half-edge structures and modifiers stay out of the core.
3. **Drag.** A gizmo drag is a param, and a compute pass applies it to the selection through an index buffer. That needs one `set_param` per frame, not 10,000 writes.
4. **Arbitrary edits.** Sculpting and tools with custom math use `write_resource` over a byte range. A pointer stream can drive a sculpt brush without JS.
5. **Normals.** A compute pass recomputes normals for affected vertices from an adjacency buffer.
6. **Draw.** Each view runs solid, edge and vertex overlays as `spatial` passes, with its own camera params and node-sized target. Four views share one mesh and one graph.
7. **Commit.** On release, logic reads the changed vertices back through `read_resource` on the buffer and updates its model. Undo belongs to the app.

A subdivision preview is one more compute pass writing a vertex buffer, in the background class.

### Other workloads in brief

| Workload | How it runs on the design | What it needed |
| --- | --- | --- |
| Compositing | Filters are a graph over tiles. A slider drag re-evaluates dirty regions, and `@reach` handles blurs | Nothing new beyond the graph |
| Video and export | Decoded frames arrive through budgeted `write_resource`. An export runs the graph at set times and reads results back | An explicit `time` on `run_graph`. Hardware decode would need the native seam |
| Plots and point clouds | 5 M points live in a storage buffer. `spatial` shaders pull them by `VERTEX_INDEX`, and compute aggregates at low zoom | Draws with no mesh |
| Volumes | A `canvas` shader marches rays through a 3D texture | 3D textures |
| Maps and CAD | Large static meshes with instancing and line overlays | Lines, wide lines, per-instance data |
| Node editors | Hundreds of user-authored shaders, each with a thumbnail, compiled visible-first | A limit on live pipelines, with eviction |

### What these walks exposed

| Gap | Workloads | Added to the design | Milestone |
| --- | --- | --- | --- |
| Byte-range buffer writes, merged per frame | Modeling, sculpting, visualization | `write_resource` with a range | M5c |
| Buffer readback | Modeling, any compute result | `read_resource` on buffers | M5d |
| An explicit time for a graph run | Video, export | `time` on `run_graph` | M5c |
| 3D and cube textures | Volumes, image-based lighting | New texture dimensions | M5b |
| Custom vertex layouts | Modeling, CAD, visualization | `attribute` in AXSL, free mesh layouts | M5e |
| Draws with no mesh | Visualization, overlays | Vertex count draws, `VERTEX_INDEX`, `INSTANCE_INDEX` | M5e |
| Lines and points | Modeling, CAD, plots | Topology in `render_mode`. WebGPU lines are one pixel, so `axe:lines` builds wide lines from quads | M5e |
| Buffers written by compute and read as vertices | Modeling, simulation | A storage-and-vertex usage flag | M5e |
| Several views of one scene | Modeling, 3D painting | Per-view params on a pass, one target per node | M5e |
| Rectangles of IDs | Box select, lasso | Region readback of `r32uint` targets | M5f |

Node-editor pipeline eviction is a lesser item, and M5h takes it with the pipeline cache.

## 3D painting: one brush stroke

A stroke can go from pen sample to pixels with no JS on its path, and this is the design's hardest test, because it combines the drawing and modeling walks. It found nine more gaps, all now in the design. The figures assume a 240 Hz pen, a 144 Hz display, 8K RGBA16F layers, 256-pixel tiles, a 64-pixel brush and a 1 M triangle mesh.

### Before the stroke

The app has built its graph once, in JS. It holds the tile pools, a page table buffer, a stroke mask, the layer stack passes and the viewport pass. A camera move re-renders the viewport's depth, world position and ID targets.

The stroke mask is one 8K `r16float` render target, 128 MiB, and it is blendable in core WebGPU. `r32float` would need `float32-blendable`. A full-size target is simple, and only fragments near the brush cost anything, because step 5 culls the triangles first.

### The stroke

1. **Input.** M4's tablet path delivers coalesced samples with pressure and tilt to the core thread. The gesture arena has already given the pointer to the viewport node.
2. **Stream.** The core maps each sample to node pixels and appends it to a stream resource bound to that node. This costs no JS. Logic gets the same events later, for status and tools.
3. **Frame demand.** New samples mark the graph's `on_stream` passes due, which requests a frame even if nothing else changed.
4. **Stamps.** A compute pass expands samples into stamps along the stroke, at 5% spacing. A fast 3,000 px/s stroke gives about 6 to 7 stamps per frame. Leftover spacing distance lives in a small storage buffer between frames.
5. **Projection and culling.** Each stamp reads the world position under it, giving a brush sphere and frame. A compute pass tests spheres against mesh clusters and writes a compacted index buffer, its indirect draw arguments and a dirty-tile list.
6. **UV-space raster.** A `spatial` pass draws the compacted triangles with `CLIP` set from `UV0`, into the stroke mask. Fragments use the interpolated world position to evaluate the brush, reject occluded points using the viewport depth, and fade by angle.
7. **Copy on write.** The first time a stroke touches a tile, a `copy` pass moves it to the undo pool. The copy is also the stroke's base, so each frame composes from base plus mask. Overlapping stamps therefore cannot build past the stroke's opacity.
8. **Layer stack.** For dirty tiles and touched channels only, compute passes run per tile from the tile list. A per-tile cache of the layers below the active one avoids re-blending them. `@reach` grows the region for any filter above that reads neighbors.
9. **Display maps.** Results copy into the viewport's display textures. A region-limited `mips` pass updates their mip chains.
10. **Viewport.** A `render` pass with `region: 'dirty'` reshades only the screen rectangle around the brush, reusing the stored depth. It writes the node's texture.
11. **Compositing.** The stream told the core the screen rectangle, so it marks that damage in the same frame. The back store redraws that rectangle, and the frame presents.

&#91;embedded content: one stroke · steps 1 to 11 across threads\]

The numbers match the steps above. Logic is dashed because a stalled JS turn does not stop the ink.

The latency target is sample to photon within 1.5 refresh periods at 144 Hz, about 10.4 ms.

At 16 dirty tiles, about 1 MPix, 7 layers' worth of blending and 4 channels, step 8 costs about 30 MPix per frame, or 4.2 GPix/s at 144 Hz. That is a design estimate, not a measurement. Without the below-layers cache it would be about 20 layers, so the cache matters. M5g measures the real number.

If steps 8 to 10 do not fit, dirty tiles stay queued, and the oldest run first. The viewport still shows the stroke mask as an overlay, so the pen never feels late even when the stack is.

### After the stroke

- **Pen up.** Logic receives the event, and reads back the tile list to build the undo record.
- **Seams.** A dilation pass with `@reach(8)` extends paint across UV island edges, in the background class.
- **Undo.** Undo swaps page-table slots back, marks those tiles dirty, and lets `inputs_changed` re-evaluate. No pixels are copied, so undo is close to instant.
- **Cancel.** A cancelled gesture is the same operation.
- **Undo memory.** The pool has its own budget. Older tiles spill to RAM through background readback, and they are compressed on the job pool.

### What the walk exposed

| Gap | Why it matters | Added to the design | Milestone |
| --- | --- | --- | --- |
| Pointer stream into a GPU buffer | JS would add a round trip at 240 Hz | `bind_pointer_stream` on a node, `on_stream` trigger | M5f |
| Region lists from the GPU | The dirty set is only known after a compute pass | `region: tiles(buffer)`, indirect dispatch | M5c |
| Buffers that are both storage and index | Compute-culled draws. WebGPU has no standard multi-draw | Usage flags on buffers, one indirect indexed draw | M5e |
| Copies over a region list | Undo copy on write per tile | `copy` pass with a tile list | M5c |
| Region-limited mips | A whole mip chain per stroke is too slow | Built-in `mips` pass | M5c |
| Scissored render passes | The viewport must not reshade everything per stamp | `region: 'dirty'` on render passes, stored depth | M5e |
| Node-sized textures | A resize would need a JS round trip | `size_from: node` | M5e |
| GPU work that overruns must keep its dirty set | Skipped frame-class work must not be forgotten | Dirty regions merge until they run | M5d |
| Device loss loses paint held only on the GPU | Driver resets happen on long sessions | Periodic background snapshot to RAM, by the app | M5h |

One limit remains by design: `binding_array` is native-only, and core WebGPU allows 16 sampled textures per stage. Tile pools must therefore fit a small number of array textures per pass.

## Project requirements

Every existing guarantee carries over: shaders and graphs render byte-identically under both schedulers, and the wire and C API evolve by the existing rules. The one thing that does not carry over is the CPU renderer, which draws a declared fallback instead of running a shader.

### Determinism

- **Time:** `TIME` and `FRAME` come from the display list, so the paused and stepped clock applies to shaders and graphs alike.
- **Readiness:** compiles and resource loads count as preparation. `ui_app_frame` waits for them, as `wait_prepared` waits for text, while a live app shows the fallback.
- **Schedulers:** the admin thread runs inline under the single scheduler, as jobs do with zero workers.
- **Slicing:** slice boundaries never come from timing in headless runs. `UI_GPU_BUDGET=fixed:N` sets a fixed slice count per frame.
- **A tested property:** a sliced run must equal an unsliced run byte for byte. The test fails any shader whose `@reach` lies.
- **Input:** A pointer test replays a recorded sample file through the stream.
- **Paused clock:** a continuous node requests no frames, because animation time has not moved.

### Golden tests

New golden tests run on lavapipe with the existing tolerance: at most 0.05% of pixels may differ by more than 2 levels. Float results can shift between Mesa versions, so goldens come from the CI image, as they do now. Tests stay within lavapipe's limits, and they test 8K behavior on regions rather than on whole textures.

### Fuzzing

| Target | Covers | Runs |
| --- | --- | --- |
| `fuzz_shader` | The compiler. Invariants: no crash, bounded memory, linear time, a monotonic source map | CI smoke and nightly |
| `fuzz_shader_device` | Compiler output through naga on lavapipe, to catch naga panics | Nightly only |
| `fuzz_graph` | Graph decoding and validation. Invariant: an accepted graph causes no WebGPU error | CI smoke and nightly |
| `fuzz_commands` | Extended with the new ops | Existing |
| `fuzz_mesh` and one per image decoder | Each decoder, when it arrives | CI smoke and nightly |

### Benchmarks

Callgrind guards CPU work only, so GPU time needs a second, local guard.

- **Callgrind cases:** `bench_shader` covers compile, reflection, graph definition, and encoding 100 shader items.
- **A zero-cost gate:** every existing case must stay within 2% when no shader is used.
- **Frame time:** `bench_gpu_live` runs under nested sway on the GTX 1650 and reports GPU time from timestamp queries. CI has no GPU, so results go in the tracked-targets table, as `bench_ui_live` does.
- **CI smoke on lavapipe:** checks counts (slices, frames, skipped passes), never times.
- **Frozen apps:** `bench/apps/shader_wall`, `filter8k`, `viewport1m`, mesh\_edit, paint\_stroke and `canvas_stroke`, under the existing freeze rules.

### The CPU reference renderer

The reference renderer cannot run WGSL, and it should not try. lavapipe already gives software execution for shader goldens. The display list stays usable by a CPU renderer through four rules:

1. Every shader and effect item carries a fallback color, the node's background by default.
2. A texture item with CPU pixels, such as a decoded image, draws normally.
3. A GPU-only texture draws its fallback.
4. Golden cases that depend on shader output are tagged `gpu` and skipped in reference mode.

### Wire protocol

New ops and events are appended under the existing rules: fixtures, `old_reader.py`, the `changes` list, and `--compat-base`. Numbers continue from `destroy_template = 23`.

| Commands | Purpose |
| --- | --- |
| `create_resource`, `destroy_resource` | Any resource, from nothing, a path or a blob |
| `write_resource`, `read_resource` | Region writes from a blob; readback by request ID |
| `set_node_texture`, `set_node_shader`, `set_node_effect` | Show GPU output in a node |
| `set_param`, `start_param_spring` | Set or animate a uniform on the core thread |
| `define_graph`, `run_graph` | Declare a graph; trigger an `on_demand` run |
| `bind_pointer_stream` | Connect a node's pointer samples to a stream |

| Events | Purpose |
| --- | --- |
| `shader_compiled` | Result, plus a blob ID for diagnostics and reflection |
| `resource_ready` | Load or create result, with size and format |
| `graph_done` | Request ID, result and GPU time |
| `readback_done` | Request ID, result and a blob ID |
| `gpu_status` | Budget warnings, device loss and recovery |
| `gpu_caps` | Features and limits, sent once at startup |

- **Graph records** are defined in the IDL, not as an ad hoc format, so they follow the `since` rules and get generated codecs.
- **Diagnostics and reflection** use blobs, since they can exceed the 65,532-byte op limit.
- **Param springs** need a dynamic list per node, because `ui_node_anim` holds a fixed `UI_SPRING_COUNT` array.

### C API

The C API gets everything TypeScript gets. Commands use generated `ui_cmd_*` encoders, and asynchronous results are request IDs.

- `ui_shader_info` and all new structs join `tests/c_api/designated.c`.
- `gpu_budget_bytes` is appended to `ui_app_desc`, and zero means the default.
- No public header gains a WebGPU type.

### TypeScript

A new `@axe/gpu` package wraps the ops: `shader()`, `texture()`, `mesh()` and `graph()`, with Promises over request IDs.

```tsx
import wave from './wave.axsl';

<View shader={wave} params={{ speed: speedSignal }} />
```

- **Typed params:** `axe-shaderc` writes a `.d.ts` for each `.axsl`, so a wrong param name fails in `tsc`.
- **Signals:** a signal bound to a param updates through `set_param`, patching one value, like any dynamic property.
- **Generated UI:** `paramPanel(wave)` builds an inspector from reflection, which is the reason for the UI hints.
- **Hot reload:** `.axsl` files join the M3b.3 watcher, and a shader edit runs no JS component.

## 3. Milestones

The work is one family, M5a to M5h, in the style of M3a to M3c. M5a is the smallest slice that proves the riskiest idea: a shader compiled off the render thread can fail or succeed without touching a frame. Each milestone is useful alone, and each stops for your review.

&#91;embedded content: milestone order · M5a to M5h, with M3c and M4\]

M5b needs M3c's back store, M5c needs its external damage, and only M5f waits for M4. M5h can move earlier if M5d shows device loss in practice.

Thresholds are proposed now, before anything runs, as M3b did. GPU figures are for the GTX 1650 at 144 Hz under nested sway. "Standard gates" means debug, release, asan and tsan suites pass, the touched fuzz targets run 5 minutes clean, and Callgrind stays within 2% for existing cases.

### M5a: Shader node (the riskiest slice)

**Contents:** the `src/shader/` compiler with `canvas` and `library` types, `axe-shaderc`, the GPU admin thread, error-scope capture and source maps, blobs, `set_node_shader`, `set_param`, `UI_DL_SHADER`, continuous frame demand, `shader_compiled`, hot reload, the device-loss fix, `fuzz_shader`, and a first `@axe/gpu`. Texture uniforms, compute and effects wait for later milestones.

**Depends on:** nothing in M3c or M4.

**Useful alone:** animated shader backgrounds and procedural UI fills, with hot reload.

**Passes when:**

- With 20 shaders compiling in a loop for 60 s, frame time p99 rises by at most 1 ms over the idle baseline, with 0 added missed refreshes.
- A corpus of 40 broken shaders reports the right file and line in 100% of cases, and the right column for every error inside a function body.
- 1,000 broken or hostile sources cause 0 uncaptured WebGPU errors and 0 device errors.
- Save to pixels takes at most 100 ms at p95 in a Debug build. Through a failed save, the last good shader keeps drawing and uniform values survive.
- A continuous node draws at refresh rate with logic idle, and draws 0 frames under a paused clock.
- Goldens at 3 stepped clock times are byte-identical under both schedulers.
- Private uniform copies cost at most 2% against a hand-rewritten version.
- Standard gates, and every `bench_*` case with no shader stays within 2%.

### M5b: Textures and effects

**Contents:** the resource table, accounting and budgets, blob and file textures in 2D, array, cube and 3D (QOI first, so no new dependency), `UI_DL_TEXTURE`, predeclared samplers, texture uniforms, the `effect` type with `BACKDROP`, `CONTENT` and `@reach`, device features and limits, `gpu_caps`, and budgeted uploads.

**Depends on:** M5a, and M3c's back store and damage inflation. Nothing in M4.

**Useful alone:** images in the UI, which Axe lacks today, plus frosted panels and color filters.

**Passes when:**

- A backdrop blur over a 1440p window costs at most 2 ms of GPU time.
- Over 1,000 random frames, a damaged redraw matches a full redraw byte for byte, with backdrop readers present.
- Creating an 8K texture leaves frame time p99 unchanged, within 1 ms.
- Over-budget and out-of-memory creation always returns an event, with 0 device errors.
- Destroying a texture in use never crashes, under asan and tsan.
- Standard gates, including the extended `fuzz_commands`.

### M5c: Compute and the graph

**Contents:** the `compute` type, `image` and `storage`, `define_graph` with validation, triggers except `on_stream`, regions, tile lists, indirect dispatch, byte-range buffer writes, an explicit run time, `copy` and `mips` passes, `@reach` dirty propagation, `graph_done`, and `fuzz_graph`.

**Depends on:** M5b, and M3c's external damage. Nothing in M4.

**Useful alone:** GPU image filters with live sliders, and generators such as noise.

**Passes when:**

- A 3-pass filter chain on a 4K texture updates within one frame during a slider drag.
- With inputs unchanged, the graph runs 0 passes and takes 0 JS turns.
- For 100 random dirty rectangles, partial evaluation matches full evaluation byte for byte.
- `fuzz_graph` holds its invariant: an accepted graph causes no WebGPU error.
- Standard gates, and goldens under both schedulers.

### M5d: Budgets and slices

**Contents:** `frame` and `background` classes, GPU timing, slicing, atomic output, cancellation, dirty regions that merge until they run, non-blocking readback of textures and buffers, a timed render wait, a fallback tick for surfaces with no frame callbacks, GPU hooks, and a GPU row in the budget table.

**Depends on:** M5c. Nothing further in M3c or M4.

**Useful alone:** a long filter never drops a UI frame.

**Passes when:**

- An 8K `rgba16float` Gaussian blur of radius 64 completes while 1,000 rows update, with at most 0.5% missed refreshes. That is the tracked target's threshold.
- Background submissions take at most 4 ms at p99, against a 2 ms target.
- A sliced run matches an unsliced run byte for byte.
- A 1 × 1 readback answers within 3 frames at p95, and the render actor never blocks longer than 0.5 ms for it.
- A job finishes on a surface that receives no frame callbacks.
- Standard gates.

### M5e: Geometry and 3D viewports

**Contents:** mesh resources with any vertex layout, draws with no mesh, lines and points, `spatial` with `attribute`, depth, MSAA, multiple targets, render passes with `region: 'dirty'`, indirect draws, buffers shared between compute and vertex or index use, per-view params, node-sized textures, `axe:pbr`, `axe:lines`, the import module, and two examples: a model viewer and a point cloud.

**Depends on:** M5d. Orbit controls feel better after M4, but scripted cameras are enough to pass. The import module needs approved dependencies.

**Useful alone:** 3D viewers in panels, plus plots and point clouds with millions of primitives.

**Passes when:**

- A 1 M triangle PBR mesh at 1440p with 4× MSAA costs at most 5 ms of GPU time.
- 5 M points from a storage buffer at 1440p cost at most 4 ms of GPU time.
- Writing 10,000 vertices of a 1 M vertex mesh costs at most 0.5 ms of core plus render CPU time per frame.
- With a deliberately slow 30 ms viewport, UI animation stays at or below 0.5% missed refreshes.
- Over 100 resizes, no frame shows a stretched viewport, with four views on screen.
- A reshade limited to a 256-pixel rectangle costs at most 15% of a full redraw.
- `spatial` goldens with MSAA, lines and points pass on lavapipe under both schedulers.
- Standard gates, including `fuzz_mesh`.

### M5f: Picking and pointer streams

**Contents:** `bind_pointer_stream`, stream resources, the `on_stream` trigger, `axe:pick`, and picking of objects, elements and rectangles.

**Depends on:** the stream needs only M5d and M4's pressure, tilt, coalesced samples and gesture arena. Picking needs M5e. So if drawing comes first, the stream half can move ahead of M5e.

**Useful alone:** object and element picking in any viewport, plus low-latency ink on a 2D canvas. It also delivers the architecture's planned inking path.

**Passes when:**

- 1,000 random picks on a golden scene return the right ID, every time.
- A box select over a 1080p rectangle answers within 5 frames at p95.
- Sample to photon is at most 1.5 refresh periods at p95, from presentation feedback.
- With logic blocked for 1 s, ink keeps drawing. This extends the `test_stall_js` idea.
- A replayed pointer stream is byte-identical under both schedulers.
- Standard gates.

### M5g: Reference proofs

**Contents:** three reference apps, each frozen under `bench/apps/`, and each its own review stop. A proof adds Axe features only where it finds a gap that passes the two-workload rule. Tile code shared by the proofs can become `@axe/tiles`.

**Depends on:** M5a to M5d and the stream half of M5f for drawing. Modeling and 3D painting also need M5e and picking.

**Useful alone:** each proof is a working reference for app authors, and you can stop after any of them.

**M5g.1 Drawing** (`examples/draw`, `bench/apps/canvas_stroke`) passes when, for a recorded 60 s stroke on an 8K, 20-layer canvas:

- Missed refreshes stay at or below 0.5%, and core plus render stay within 6.9 ms.
- Sample to photon is at most 10.4 ms at p95.
- Pan and zoom hold refresh rate and re-evaluate 0 layers.
- Undo takes one frame, and GPU memory stays under 3 GiB.

**M5g.2 Modeling** (`examples/modeler`, `bench/apps/mesh_edit`) passes when, on a 1 M vertex mesh in four views with all overlays:

- Dragging 10,000 vertices stays at or below 0.5% missed refreshes.
- A click pick answers within 3 frames at p95, and a box select within 5.
- Committing an edit reads back and applies within 3 frames at p95.
- A reduced-size replay is byte-identical under both schedulers on lavapipe.

**M5g.3 3D painting** (`examples/paint`, `bench/apps/paint_stroke`) passes when, for a recorded 60 s stroke on an 8K, 20-layer, 1 M triangle project:

- Missed refreshes stay at or below 0.5%, and core plus render stay within 6.9 ms.
- Sample to viewport pixels is at most 10.4 ms at p95.
- Undo takes one frame.
- GPU memory stays under 3 GiB.
- A reduced-size replay is byte-identical under both schedulers on lavapipe.

### M5h: Recovery and polish

**Contents:** device-loss recovery, a `gpu_status` event, rebuilt pipelines, resources reloaded from their sources, and the pipeline disk cache behind its capability. Resources with no source are reported as lost, and apps restore them from snapshots.

**Depends on:** M5a to M5d. It can move earlier if M5d shows device loss in practice.

**Useful alone:** driver resets stop ending the session.

**Passes when:**

- Over 100 injected device losses, the app never crashes, and the UI returns within 1 s.
- The pipeline cache cuts second-launch compile time by at least 80%.
- Standard gates.

## 4. Changes to the current plan

M3c needs seven additions now, and one of them, the back store, is structural. They add little to M3c, and retrofitting them after an incremental display list exists would mean reworking it. The recommended order changes too: M5a first, then M3c, then M4 and M5b onward in parallel.

### Why M5a goes before M3c

M5a depends on nothing in M3c, and it stays small. Its continuous node then gives M3c a real test case: an always-changing node that today's scene cannot produce. If you prefer M3c first, the design still works, and M3c would test external damage with a fake resource version instead.

### What M3c must do now

| Change | Why | Cost of deferring |
| --- | --- | --- |
| **A persistent back store per surface.** Render into a retained texture, then blit to the swapchain | Partial redraw needs last frame's pixels, and `encode` clears the swapchain each frame. It also enables backdrop reads, window capture, one pipeline format and later an HDR format | M3c's damage model would have to be replaced |
| **External damage.** Allow an edge from resource version to node, with a sub-rect | GPU output changes with no scene change | Damage logic rewritten around a second input |
| **Damage inflation.** Reserve a per-node `reads_backdrop(radius)` | A backdrop reader depends on everything under it | Occlusion and layer caching would be wrong for effects |
| **Frame demand as reasons.** Replace `root->animating` with a reason mask and an active list | The current scan is O(all nodes), which M3c must remove anyway. Continuous nodes and GPU work need to join | Scene changes twice |
| **Pinned resources in retained lists.** Generalize `protect_epoch` | A retained list segment can name a texture for many frames | Use-after-free risk across threads |
| **`gpu_seq` beside `atlas_seq`.** Keep the FIFO rule | The mailbox may replace a list, so work cannot ride on it | Lost GPU work on replaced lists |
| **Extensible batch kinds.** Treat list segments as able to hold pipeline changes and pass breaks | `UI_DL_SHADER` and `UI_DL_EFFECT` follow soon | Incremental encoding reworked |

The back store costs one full-surface texture, about 32 MiB at 4K RGBA8, and one blit per drawn frame. An sRGB round trip through the blit must be exact: M3c should prove it with a byte-identical golden run.

Two rules follow for layer caching: a subtree holding a continuous node is not cached across, and that node gets a list segment of its own, so its neighbors never re-encode.

### Check first: M3c entry tests

Before M3c builds on them, three facts need a quick test, because the code cannot settle them:

1. The back-store blit adds no visible latency and passes byte-identical goldens.
2. Partial redraw works on `BGRA8UnormSrgb` and `RGBA8UnormSrgb` targets alike.
3. `wl_surface.damage` helps or not on KWin once the whole swapchain image is redrawn each frame.

### Architecture rules that change

| Rule today | Proposed | Why |
| --- | --- | --- |
| All GPU work happens on the render thread | Encoding, submission and surface calls stay on the render actor. Object creation may run on the one GPU admin thread, also under `src/render/` | No async creation exists in wgpu-native |
| Every WebGPU call is in `render.c` | Split into `device.c`, `encode.c`, `resource.c`, `graph.c`, `admin.c`. `webgpu_checked.h` allows two scopes | The file would otherwise grow to many times its size |
| All pipeline variants are precompiled at startup | True for built-ins. User shaders compile at load, with a disk cache | Runtime shaders cannot be known at startup |
| Render extensions are custom WebGPU passes | Passes are data first. Native extensions are an internal seam until a package needs them | Keeps the ABI out of wgpu-native's release cycle |
| Damage tracking limits redraws to changed regions | Adds external damage and backdrop inflation | GPU output changes outside the scene |
| Budget table lists core + render CPU time | Adds a GPU row, per surface refresh | Compute can drop frames with idle CPUs |
| Fuzz every boundary | Adds shaders, graph blobs and meshes by name | They are new external input |

These rules stay as they are: the standard `webgpu.h` target, no third-party types in public headers, a renderer-agnostic display list, and no JS on the frame path.

### Documents

The architecture's milestone table gains M5a to M5h. New `docs/shaders.md` and `docs/gpu.md` document the language and the GPU API. `docs/decisions.md` gets a section per milestone, as now. CONTRIBUTING gains the new fuzz targets and the `gpu` golden tag.

Nothing in this section has been changed in the repository.

## 5. Risks, open questions and dependencies

The largest unknown is whether pipeline creation on the admin thread can block queue submission through locks inside wgpu. Reading the code cannot settle that, so M5a's first pass criterion measures it. Nothing after M5a should start until it passes.

### Risks

| Risk | Impact | Mitigation | Checked in |
| --- | --- | --- | --- |
| wgpu locks make pipeline creation block submission | Compiles stall frames, and the admin thread loses its point | Measure first. Fallback: compile shader modules off-thread and pipelines in budgeted render-thread windows, relying on driver caches | M5a |
| A naga panic aborts the process, since panics cannot cross the FFI boundary | A hostile shader kills the app | Input limits, `fuzz_shader_device`. Last resort: compile in a helper process | M5a, nightly |
| A valid shader hangs the GPU, for example with an unbounded loop | Device loss | No compile-time cure. Slices limit honest work, and M5h recovers. See the strict mode question below | M5d, M5h |
| Real device loss is ignored today | The app keeps running with dead GPU objects | Fix in M5a, before any heavy shader exists | M5a |
| Calling an `unimplemented!()` function aborts the process | One wrong call ends the app | `webgpu_checked.h` generator refuses those names at build time | M5a |
| Error scope cross-talk | A render-thread bug is blamed on a shader | Only the admin thread uses scopes; the render actor never does | M5a |
| naga's text format changes on a wgpu upgrade | Diagnostics lose positions | Corpus test fails in CI; full text still reaches the user; a Rust shim is the fallback | Each upgrade |
| Two Rust static libraries may clash at link time, if the shim is ever needed | A build problem | Try it only if text parsing fails | If needed |
| GPU preemption is too coarse, even at 2 ms slices | Occasional missed refreshes | Measure, then shrink slices. Report through hooks | M5d |
| Only the Vulkan backend is built | HLSL and MSL paths are untested, and Windows has a driver timeout near 2 s | Short slices help. Retest each backend when ports begin | Ports |
| Mesa updates shift float results | Golden churn | Generate goldens from the pinned CI image | Ongoing |
| 8K tests exhaust the dev machine's 15 GiB | OOM kills, as in 3b.2 | Run under `MemoryMax`, and test 8K behavior on regions | All |
| Scope grows toward a game engine | The core gets heavy | The test in section 3D, applied in review | All |
| The family is large for one developer | Long gap before the first reference proof | Every milestone is useful alone, and you can stop after any of them | All |
| One app's needs shape the core | APIs fit that app and block the others | The two-workload rule, and three proofs that share no geometry or tile assumptions | All |

### Open questions

- [ ] Should editing layers stay in `rgba16float`, or does color banding demand `rgba32float` or `rgba16unorm`? M5g can compare them on a gradient.
- [ ] Will Axe run shaders from strangers, such as a marketplace? If so, it needs a strict mode that rejects unbounded loops, and possibly helper-process compilation.
- [ ] Is 2 GiB the right default GPU budget? M5d can tune it. Note that no standard API reports VRAM size.
- [ ] Should tile logic become `@axe/tiles`, or stay as example code? Decide after M5g.
- [ ] Are the names AXSL and `.axsl` acceptable?
- [ ] When does HDR surface output matter? The back store allows it, but Wayland color management is not in these milestones.
- [ ] Should M5f's stream replace the architecture's planned render-thread inking path, or feed it? I recommend replacing it, because it has no special case.
- [ ] Is ThorVG due before M5e? It can use M5b's texture resources and the internal seam at any point after M5b.

### Proposed dependencies

None is added by this plan, and M5a needs none. Licenses are from memory, so verify each one before vendoring. Change the Approval cell to decide.

| Candidate | Purpose | License (from memory) | Needed by | Without it | Approval |
| --- | --- | --- | --- | --- | --- |
| cgltf | glTF 2.0 parsing, C, single header | MIT | M5e | Write our own parser | Pending |
| MikkTSpace | Tangents matching the bakers' convention | zlib-style | M5e | Normal maps from other tools shade wrongly | Pending |
| fast\_obj | OBJ parsing | MIT | M5e | About 400 lines of our own, which I prefer | Pending |
| libspng, plus zlib or miniz | PNG. Planned in the architecture, not yet vendored | BSD-2 | M5b, optional | QOI-only images until M5e | Pending |
| libjpeg-turbo | JPEG. Planned in the architecture, not yet vendored | BSD-style | M5e | No JPEG textures | Pending |
| meshoptimizer | Vertex cache order and clusters | MIT | M5g, optional | Our own UV-tile clustering | Pending |
| LZ4 | Undo tile compression | BSD-2 | M5g, optional | Spill undo tiles uncompressed, or a QOI-style codec of our own | Pending |
| ufbx | FBX import | MIT | After M5g | Users convert to glTF | Pending |
| tinyexr | OpenEXR environment maps and export | BSD-3 | After M5g | Our own Radiance `.hdr` decoder for input, no EXR | Pending |
| naga, direct use in a Rust shim | Structured shader diagnostics. Already vendored through wgpu-native | MIT / Apache-2.0 | Only if text parsing fails | Parse naga's message text | Pending |

No Rust toolchain change is needed, and enabling features in wgpu-native adds no crates.

## Addendum: decisions since this design

Nov 5, 2026. The design above is kept as it was reviewed. Where it differs from this list, this list wins.

| Decision | What it replaces | Consequence |
| --- | --- | --- |
| The seven foundations go in M3c, with the three entry tests (section 4) | Nothing: this takes the recommendation | M3c lays the back store, external damage, damage inflation, frame demand as reasons, pinned resources, `gpu_seq` and extensible batch kinds |
| A spike runs before M3c | The first pass criterion of M5a being the first measurement of the riskiest idea | Device loss is fixed and off-thread creation is measured before M3c builds on the plan. Results: [decisions.md](decisions.md), "GPU spike" |
| M5a comes after M3c | Decision 1, "Run M5a before M3c? Yes" | The order is M3b.5, the spike, M3c, M5a, M4, M5b to M5h. M3c tests external damage with a fake resource version, as "Why M5a goes before M3c" allows. M5a no longer contains the device-loss fix |
| `TIME` wraps at a constant 3,600 s | Nothing: this keeps the value in "Shader types" | One named constant, the same for every shader, so a wrap can't differ between surfaces or schedulers |
| Shaders from strangers stay an open question | Nothing: it stays open under "Open questions" | No strict mode and no helper-process compilation are planned until it's answered |
| M5a's save-to-pixels criterion names a reference shader | "Save to pixels takes at most 100 ms at p95", which names no shader | The figure can't hold for every shader: in the spike, a shader of about 600 lines took about a second for its two pipelines. M5a names the shader in the criterion before measuring it |
| The admin queue has priorities and cancellation | Nothing: the design gives the queue no order | One shader can hold the admin thread for seconds, and cost grows faster than size. A hot reload mustn't wait behind a build, and a compile that a newer save replaced is dropped |
| Module errors are reported before pipelines finish | "The admin thread validates it and builds pipelines", as one step | Modules are 30 to 100 times cheaper than pipelines, so a shader's errors reach `shader_compiled` in milliseconds, while its pipelines build behind them |
| The first draw with a new pipeline is measured first | Nothing: the spike didn't cover it | The spike's bench never drew with what it created, and a driver may do lazy work at the first bind, on the render thread. This is M5a's first measurement, as the admin-thread rule only holds if that stays within budget |
| The pipeline disk cache moves from M5h to M5a | M5h's contents, and its criterion that the cache cuts second-launch compile time by at least 80% | M5a takes both, and M5h keeps rebuilding pipelines after a loss. See the note below: wgpu-native can't create a pipeline cache yet |
| A window on lavapipe isn't judged by M5a's first criterion | "With 20 shaders compiling in a loop for 60 s, frame time p99 rises by at most 1 ms" with no platform named | The criterion applies on a GPU. On lavapipe the miss is CPU contention in a software renderer, not a lock: a thread that makes no GPU calls costs more |

**The pipeline cache has no API yet.** At the pinned v29.0.1.1 and on trunk, wgpu-native exposes `WGPUNativeFeature_PipelineCache` and no function that creates a cache, and [the pull request that adds one](https://github.com/gfx-rs/wgpu-native/pull/618) is a draft. The 45 to 200 times the spike measured came from NVIDIA's own disk cache, not from anything Axe controls, and lavapipe caches nothing. So M5a's first job on the cache is choosing how to build it: take that pull request or a patch of ours once it can be tested, or do without one.

The spike also corrected three statements above. Details are in [decisions.md](decisions.md), "GPU spike".

- **"A bug to fix first"** says a real loss looks silently ignored. It's worse: if the driver reports the loss from inside submit or acquire, wgpu-native panics, and that aborts the process. We build wgpu-native with [a patch](https://github.com/gfx-rs/wgpu-native/pull/641) for this until a release includes it.
- **The same section** says wgpu-native reports a loss with the reason `Destroyed`. It does for few calls: most report a validation error, and the callback never fires on its own.
- **The largest unknown in section 5** is answered: no wgpu lock is held across a compile. See the results for what the measurement did find.
