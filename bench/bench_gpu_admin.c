// Does creating shader modules and pipelines on a second thread stall frames, through locks
// inside wgpu or the driver? The GPU layer's admin thread depends on the answer
// (docs/gpu-design.md, "Risks"; the result is in docs/decisions.md, "GPU spike").
//
// A real window runs bench/apps/list10k streaming, through the real renderer. In `admin` mode a
// second thread creates objects on the renderer's device for as long as the window measures:
// 20 shaders in a loop, 15 small and 5 large, each a shader module, a render pipeline and a
// compute pipeline. Every compile is new: all constants change each time, and the shader's
// output depends on them, so no cache recognizes it. `baseline` mode has no second thread.
//
//   bench_gpu_admin [--mode baseline|admin|fallback|render|burn] [--seconds S] [--rows=N] [--same]
//                   [--large-steps N] [--budget-ms MS] [--label TEXT] [--json FILE] [bundle.axbc]
//
// --same repeats identical sources, a control that shows what caches would have hidden.
// `fallback` creates only modules off the render thread, and pipelines on it, in a window after
// present: a shader's pipelines start only if their predicted cost fits --budget-ms.
// `render` is the same with no budget, every pipeline in the first window it reaches: the
// control that shows this bench can see a stall. `burn` spins on the second thread and calls
// no GPU function: it tells CPU contention from locks, on a software rasterizer above all.
//
// Run it in a compositor with a fixed refresh rate, through bench/run_gpu_admin.sh. On one line
// of JSON (--json) or a page of text, it reports:
//  - frame: FRAME_BEGIN to FRAME_END, the core's and render's time on a frame. A stall in a GPU
//    call lands here, whatever the display does, which is why the pass criterion reads it;
//  - interval: between the compositor's present times, which come in whole refreshes;
//  - missed: refreshes with no new frame, counted as bench_ui_live counts them;
//  - acquire, submit, swap: single GPU calls, from the phases of the same names;
//  - compile: how long each object took on the second thread.
#include <inttypes.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#include <webgpu.h>

#include "runtime/app_internal.h"
#include "runtime/common.h"
#include "ui/ui.h"

#define WARMUP_NS 3000000000ull // the mount and the first shaping, left out
#define MAX_SAMPLES 40000u      // over a minute at 144 Hz, several times over
#define SHADER_COUNT 20u
#define LARGE_EVERY 4u          // every 4th shader of the 20 is large
#define MAX_WGSL (1024u * 1024u)
#define FRAME_SLOTS 1024u
#define PROBES 2000

typedef struct sample {
    uint64_t t, value;
} sample;

typedef struct series {
    sample *samples;
    _Atomic uint32_t count;
} series;

enum { S_FRAME, S_INTERVAL, S_ENCODE, S_ACQUIRE, S_SUBMIT, S_SWAP, S_PRESENT, S_COUNT };
static const char *const series_names[S_COUNT] = {"frame", "interval", "encode", "acquire",
                                                  "submit", "swap", "present"};

enum { C_MODULE, C_RENDER, C_COMPUTE, C_KINDS };
enum { SMALL, LARGE, SIZES };
static const char *const compile_names[C_KINDS] = {"module", "render", "compute"};

static struct {
    ui_app *app;
    WGPUDevice device;
    uint64_t start, seconds, end;
    bool same, fallback, unbudgeted;
    uint64_t budget_ns;
    uint32_t large_steps;
    series series[S_COUNT];
    _Atomic uint64_t frame_begin[FRAME_SLOTS];
    uint64_t phase_begin[UI_PHASE_COUNT]; // render thread
    uint64_t last_present;
    uint64_t refresh_ns;
    uint64_t frames, missed; // inside the measured window
    _Atomic bool stop;
    _Atomic uint32_t lost;
    uint64_t compile[C_KINDS][SIZES][MAX_SAMPLES]; // second thread
    uint32_t compile_count[C_KINDS][SIZES];
    uint64_t compile_errors;
} run;

static void series_add(series *s, uint64_t t, uint64_t value) {
    uint32_t n = atomic_load_explicit(&s->count, memory_order_relaxed);
    if (n >= MAX_SAMPLES) return;
    s->samples[n] = (sample){t, value};
    atomic_store_explicit(&s->count, n + 1, memory_order_release);
}

static void pipelines_after_present(void);

static bool measuring(uint64_t t) { return t >= run.start + WARMUP_NS && t < run.end; }

static void on_trace(void *user, const ui_trace_event *e) {
    (void)user;
    switch (e->kind) {
    case UI_TRACE_FRAME_BEGIN:
        atomic_store(&run.frame_begin[e->frame_id % FRAME_SLOTS], e->time_ns);
        if (e->time_ns >= run.end) ui_app_quit(run.app);
        break;
    case UI_TRACE_FRAME_END: {
        uint64_t begin = atomic_load(&run.frame_begin[e->frame_id % FRAME_SLOTS]);
        if (begin && measuring(begin)) series_add(&run.series[S_FRAME], begin, e->time_ns - begin);
        if (run.fallback) pipelines_after_present();
        break;
    }
    case UI_TRACE_PHASE_BEGIN:
        run.phase_begin[e->phase] = e->time_ns;
        break;
    case UI_TRACE_PHASE_END: {
        int s = e->phase == UI_PHASE_ENCODE    ? S_ENCODE
                : e->phase == UI_PHASE_ACQUIRE ? S_ACQUIRE
                : e->phase == UI_PHASE_SUBMIT  ? S_SUBMIT
                : e->phase == UI_PHASE_SWAP    ? S_SWAP
                : e->phase == UI_PHASE_PRESENT ? S_PRESENT
                                               : -1;
        if (s >= 0 && measuring(e->time_ns))
            series_add(&run.series[s], e->time_ns, e->time_ns - run.phase_begin[e->phase]);
        break;
    }
    case UI_TRACE_PRESENTED:
        if (!e->time_ns) break;
        if (e->refresh_ns) run.refresh_ns = e->refresh_ns;
        if (run.last_present && measuring(run.last_present) && run.refresh_ns) {
            uint64_t gap = e->time_ns - run.last_present;
            uint64_t refreshes = (gap + run.refresh_ns / 2) / run.refresh_ns;
            series_add(&run.series[S_INTERVAL], e->time_ns, gap);
            run.frames++;
            if (refreshes > 1) run.missed += refreshes - 1;
        }
        run.last_present = e->time_ns;
        break;
    case UI_TRACE_DEVICE_LOST:
        atomic_fetch_add(&run.lost, 1);
        break;
    default:
        break;
    }
}

// ---- Generated shaders -------------------------------------------------------------------

typedef struct text {
    char *data;
    size_t size;
} text;

[[gnu::format(printf, 2, 3)]] static void put(text *t, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int n = vsnprintf(t->data + t->size, MAX_WGSL - t->size, format, args);
    va_end(args);
    if (n < 0 || (size_t)n >= MAX_WGSL - t->size) abort(); // MAX_WGSL is too small
    t->size += (size_t)n;
}

// A constant no compile has used. xorshift: reproducible, and spread over all the digits.
static float fresh(uint64_t *state) {
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return 0.25f + (float)(x % 1000003u) / 1000003.0f;
}

static void shader_prelude(text *t) {
    put(t, "struct Globals { size: vec2f, time: f32, opacity: f32, tint: vec4f }\n"
           "@group(0) @binding(0) var<uniform> globals: Globals;\n"
           "@group(0) @binding(1) var<storage, read_write> cells: array<vec4f>;\n"
           "struct VsOut { @builtin(position) pos: vec4f, @location(0) uv: vec2f }\n"
           "@vertex fn vs_main(@builtin(vertex_index) i: u32) -> VsOut {\n"
           "    let uv = vec2f(f32(i & 1u), f32((i >> 1u) & 1u));\n"
           "    return VsOut(vec4f(uv * 2.0 - 1.0, 0.0, 1.0), uv);\n"
           "}\n");
}

static void shader_entries(text *t) {
    put(t, "@fragment fn fs_main(in: VsOut) -> @location(0) vec4f {\n"
           "    return shade(in.uv) * globals.opacity;\n"
           "}\n"
           "@compute @workgroup_size(8, 8) fn cs_main(@builtin(global_invocation_id) id: vec3u) {\n"
           "    cells[id.x + id.y * 256u] = shade(vec2f(id.xy) / globals.size);\n"
           "}\n");
}

// About what a canvas shader in the design looks like.
static void small_shader(text *t, uint64_t *state) {
    shader_prelude(t);
    put(t, "const K0: f32 = %.9f;\nconst K1: f32 = %.9f;\nconst K2: vec2f = vec2f(%.9f, %.9f);\n",
        fresh(state), fresh(state), fresh(state), fresh(state));
    put(t, "fn shade(uv: vec2f) -> vec4f {\n"
           "    let wave = 0.5 + 0.5 * sin(uv.x * 12.0 * K0 + globals.time * K1);\n"
           "    let ring = smoothstep(0.2, 0.8, length(uv - K2));\n"
           "    return mix(globals.tint, vec4f(uv, ring, 1.0), wave * K1);\n"
           "}\n");
    shader_entries(t);
}

// A chain of steps, each feeding the next so that none is dead code, the size of a shader
// generated by a node graph or carrying a material library.
static void large_shader(text *t, uint64_t *state) {
    shader_prelude(t);
    for (uint32_t i = 0; i < run.large_steps; i++) {
        put(t, "const C%u: vec4f = vec4f(%.9f, %.9f, %.9f, %.9f);\n", i, fresh(state), fresh(state),
            fresh(state), fresh(state));
        put(t, "fn step%u(v: vec4f, uv: vec2f) -> vec4f {\n", i);
        if (i) put(t, "    var p = step%u(v, uv) * C%u + vec4f(uv, globals.time, 1.0);\n", i - 1, i);
        else put(t, "    var p = v * C%u + vec4f(uv, globals.time, 1.0);\n", i);
        put(t, "    for (var n = 0; n < 4; n++) {\n"
               "        p = mix(sin(p * C%u.x), cos(p.wzyx + C%u), fract(dot(p, C%u)));\n"
               "        if (p.x > C%u.y) {\n"
               "            p = p.yzwx * 0.5;\n"
               "        } else {\n"
               "            p = abs(p) + C%u.zzzz;\n"
               "        }\n"
               "    }\n"
               "    let q = normalize(p + vec4f(0.001));\n"
               "    return q * pow(abs(q.x) + 1.0, 1.0 + C%u.w);\n"
               "}\n", i, i, i, i, i, i);
    }
    put(t, "fn shade(uv: vec2f) -> vec4f { return step%u(globals.tint, uv); }\n", run.large_steps - 1);
    shader_entries(t);
}

// ---- The second thread -------------------------------------------------------------------

static WGPUStringView sv(const char *s) { return (WGPUStringView){s, WGPU_STRLEN}; }

static void on_scope(WGPUPopErrorScopeStatus status, WGPUErrorType type, WGPUStringView message,
                     void *user1, void *user2) {
    (void)user2;
    if (status == WGPUPopErrorScopeStatus_Success && type == WGPUErrorType_NoError) return;
    if (!(*(uint64_t *)user1)++) fprintf(stderr, "compile error: %.*s\n", (int)message.length, message.data);
}

static void pop_scope(void) {
    WGPUPopErrorScopeCallbackInfo info = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
    info.mode = WGPUCallbackMode_AllowSpontaneous;
    info.callback = on_scope;
    info.userdata1 = &run.compile_errors;
    wgpuDevicePopErrorScope(run.device, info);
}

static WGPUShaderModule create_module(const text *t) {
    WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
    wgsl.code = (WGPUStringView){t->data, t->size};
    WGPUShaderModuleDescriptor desc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    desc.nextInChain = &wgsl.chain;
    desc.label = sv("bench");
    return wgpuDeviceCreateShaderModule(run.device, &desc);
}

static WGPURenderPipeline create_render(WGPUPipelineLayout layout, WGPUShaderModule module) {
    WGPUBlendState blend = WGPU_BLEND_STATE_INIT;
    blend.color = (WGPUBlendComponent){WGPUBlendOperation_Add, WGPUBlendFactor_One,
                                       WGPUBlendFactor_OneMinusSrcAlpha};
    blend.alpha = blend.color;
    WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
    target.format = WGPUTextureFormat_BGRA8UnormSrgb;
    target.blend = &blend;
    target.writeMask = WGPUColorWriteMask_All;
    WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
    fragment.module = module;
    fragment.entryPoint = sv("fs_main");
    fragment.targetCount = 1;
    fragment.targets = &target;
    WGPURenderPipelineDescriptor desc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    desc.label = sv("bench");
    desc.layout = layout;
    desc.vertex.module = module;
    desc.vertex.entryPoint = sv("vs_main");
    desc.primitive.topology = WGPUPrimitiveTopology_TriangleStrip;
    desc.fragment = &fragment;
    return wgpuDeviceCreateRenderPipeline(run.device, &desc);
}

static WGPUComputePipeline create_compute(WGPUPipelineLayout layout, WGPUShaderModule module) {
    WGPUComputePipelineDescriptor desc = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
    desc.label = sv("bench");
    desc.layout = layout;
    desc.compute.module = module;
    desc.compute.entryPoint = sv("cs_main");
    return wgpuDeviceCreateComputePipeline(run.device, &desc);
}

static void note_compile(int kind, int size, uint64_t ns) {
    uint32_t *n = &run.compile_count[kind][size];
    if (*n < MAX_SAMPLES) run.compile[kind][size][(*n)++] = ns;
}

// The render thread's side of `fallback`: modules made on the second thread wait here for it
// to make their pipelines, in a window after present. One producer, one consumer.
#define PENDING_SLOTS 4u
typedef struct pending {
    WGPUShaderModule module;
    int size;
} pending;
static struct {
    pending slots[PENDING_SLOTS];
    _Atomic uint32_t head, tail;
    WGPUPipelineLayout layout;
    uint64_t predicted_ns[SIZES]; // recent cost of a shader's two pipelines
    uint32_t waited;              // windows the head has waited, to keep it from starving
    uint64_t ran, deferred, forced;
} after_present;

// A frame has just finished on the render thread: that's the window. What no budget can fit
// still has to run some time, so the head waits at most 8 windows and then runs anyway.
static void pipelines_after_present(void) {
    uint32_t head = atomic_load(&after_present.head);
    if (atomic_load(&run.stop) || head == atomic_load(&after_present.tail)) return;
    pending p = after_present.slots[head % PENDING_SLOTS];
    bool fits = run.unbudgeted || after_present.predicted_ns[p.size] <= run.budget_ns;
    if (!fits && ++after_present.waited < 8) {
        after_present.deferred++;
        return;
    }
    if (!fits) after_present.forced++;
    after_present.waited = 0;
    atomic_store(&after_present.head, head + 1);
    after_present.ran++;
    uint64_t t0 = ui_monotonic_ns();
    wgpuDevicePushErrorScope(run.device, WGPUErrorFilter_Validation);
    WGPURenderPipeline render = create_render(after_present.layout, p.module);
    pop_scope();
    uint64_t t1 = ui_monotonic_ns();
    wgpuDevicePushErrorScope(run.device, WGPUErrorFilter_Validation);
    WGPUComputePipeline compute = create_compute(after_present.layout, p.module);
    pop_scope();
    uint64_t t2 = ui_monotonic_ns();
    note_compile(C_RENDER, p.size, t1 - t0);
    note_compile(C_COMPUTE, p.size, t2 - t1);
    after_present.predicted_ns[p.size] = t2 - t0;
    wgpuComputePipelineRelease(compute);
    wgpuRenderPipelineRelease(render);
    wgpuShaderModuleRelease(p.module);
}

// Busy on one core, on what the compile thread does outside the driver: writing shaders.
static int burn_thread(void *arg) {
    (void)arg;
    text source = {malloc(MAX_WGSL), 0};
    uint64_t state = 0x9E3779B97F4A7C15ull;
    while (!atomic_load(&run.stop)) {
        source.size = 0;
        large_shader(&source, &state);
    }
    free(source.data);
    return 0;
}

static int compile_thread(void *arg) {
    (void)arg;
    WGPUBindGroupLayoutEntry entries[2] = {WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT,
                                           WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT};
    entries[0].binding = 0;
    entries[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment | WGPUShaderStage_Compute;
    entries[0].buffer.type = WGPUBufferBindingType_Uniform;
    entries[1].binding = 1;
    entries[1].visibility = WGPUShaderStage_Compute;
    entries[1].buffer.type = WGPUBufferBindingType_Storage;
    WGPUBindGroupLayoutDescriptor bgl = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    bgl.entryCount = 2;
    bgl.entries = entries;
    WGPUBindGroupLayout group = wgpuDeviceCreateBindGroupLayout(run.device, &bgl);
    WGPUPipelineLayoutDescriptor pl = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    pl.bindGroupLayoutCount = 1;
    pl.bindGroupLayouts = &group;
    WGPUPipelineLayout layout = wgpuDeviceCreatePipelineLayout(run.device, &pl);
    after_present.layout = layout;

    text source = {malloc(MAX_WGSL), 0};
    uint64_t serial = 1;
    while (!atomic_load(&run.stop)) {
        for (uint32_t i = 0; i < SHADER_COUNT && !atomic_load(&run.stop); i++, serial++) {
            int size = i % LARGE_EVERY == LARGE_EVERY - 1 ? LARGE : SMALL;
            // Seeded per process too: the driver's disk cache outlives a run. With --same,
            // shader i is the same every time round.
            uint64_t state = run.same ? 0x9E3779B97F4A7C15ull * (i + 1)
                                      : 0x9E3779B97F4A7C15ull * serial ^ run.start;
            source.size = 0;
            if (size == LARGE) large_shader(&source, &state);
            else small_shader(&source, &state);

            // Scopes as the design's admin thread uses them (docs/gpu-design.md, "Errors").
            uint64_t t0 = ui_monotonic_ns();
            wgpuDevicePushErrorScope(run.device, WGPUErrorFilter_Validation);
            WGPUShaderModule module = create_module(&source);
            pop_scope();
            uint64_t t1 = ui_monotonic_ns();
            note_compile(C_MODULE, size, t1 - t0);
            if (run.fallback) {
                // Wait for the render thread to take it.
                for (;;) {
                    uint32_t tail = atomic_load(&after_present.tail);
                    if (tail - atomic_load(&after_present.head) < PENDING_SLOTS) {
                        after_present.slots[tail % PENDING_SLOTS] = (pending){module, size};
                        atomic_store(&after_present.tail, tail + 1);
                        break;
                    }
                    if (atomic_load(&run.stop)) {
                        wgpuShaderModuleRelease(module);
                        break;
                    }
                    thrd_sleep(&(struct timespec){.tv_nsec = 500000}, NULL);
                }
                continue;
            }
            wgpuDevicePushErrorScope(run.device, WGPUErrorFilter_Validation);
            WGPURenderPipeline render = create_render(layout, module);
            pop_scope();
            uint64_t t2 = ui_monotonic_ns();
            note_compile(C_RENDER, size, t2 - t1);
            wgpuDevicePushErrorScope(run.device, WGPUErrorFilter_Validation);
            WGPUComputePipeline compute = create_compute(layout, module);
            pop_scope();
            uint64_t t3 = ui_monotonic_ns();
            note_compile(C_COMPUTE, size, t3 - t2);
            wgpuComputePipelineRelease(compute);
            wgpuRenderPipelineRelease(render);
            wgpuShaderModuleRelease(module);
        }
    }
    free(source.data);
    // The render thread may still be creating pipelines, so `fallback` keeps these to the end.
    if (!run.fallback) {
        wgpuPipelineLayoutRelease(layout);
        wgpuBindGroupLayoutRelease(group);
    }
    return 0;
}

// ---- Report ------------------------------------------------------------------------------

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

typedef struct percentiles {
    uint32_t n;
    double p50, p99, max;
} percentiles;

static percentiles percentiles_of(uint64_t *values, uint32_t n) {
    percentiles p = {.n = n};
    if (!n) return p;
    qsort(values, n, sizeof *values, cmp_u64);
    p.p50 = (double)values[n / 2] / 1e6;
    p.p99 = (double)values[(uint64_t)(n - 1) * 99 / 100] / 1e6;
    p.max = (double)values[n - 1] / 1e6;
    return p;
}

int main(int argc, char **argv) {
    const char *bundle = UI_LIST10K_AXBC, *mode = "baseline", *label = "", *json = NULL;
    run.seconds = 60;
    run.large_steps = 40;
    const char *args[2] = {"--stream", NULL};
    uint32_t arg_count = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc) run.seconds = (uint64_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--mode") && i + 1 < argc) mode = argv[++i];
        else if (!strcmp(argv[i], "--label") && i + 1 < argc) label = argv[++i];
        else if (!strcmp(argv[i], "--json") && i + 1 < argc) json = argv[++i];
        else if (!strcmp(argv[i], "--large-steps") && i + 1 < argc) run.large_steps = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--budget-ms") && i + 1 < argc) run.budget_ns = (uint64_t)(atof(argv[++i]) * 1e6);
        else if (!strcmp(argv[i], "--same")) run.same = true;
        else if (!strncmp(argv[i], "--rows=", 7)) args[arg_count++] = argv[i];
        else bundle = argv[i];
    }
    bool second_thread = strcmp(mode, "baseline") != 0;
    run.unbudgeted = !strcmp(mode, "render");
    run.fallback = run.unbudgeted || !strcmp(mode, "fallback");
    for (int i = 0; i < S_COUNT; i++) run.series[i].samples = malloc(MAX_SAMPLES * sizeof(sample));

    static const char *const fonts[] = {AXE_SOURCE_DIR "/tests/fonts/NotoSans-VF.ttf"};
    ui_script_desc script = {.bundle_path = bundle, .args = args, .arg_count = arg_count, .long_task_ns = UINT64_MAX / 2};
    ui_app_desc desc = {
        .scheduler = UI_SCHEDULER_THREADED,
        .surface = {.kind = UI_SURFACE_WINDOW, .width = 800, .height = 600, .title = "bench_gpu_admin"},
        .script = &script,
        .text = {.font_files = fonts, .font_file_count = 1},
    };
    if (ui_app_create(&desc, &run.app) != UI_OK) {
        fprintf(stderr, "can't create the app (a Wayland compositor is needed)\n");
        return 1;
    }
    run.device = ui_render_native_device(run.app->render);
    // What the renderer's device_alive() costs, twice a frame.
    uint64_t probe_begin = ui_monotonic_ns();
    for (int i = 0; i < PROBES; i++) wgpuCommandEncoderRelease(wgpuDeviceCreateCommandEncoder(run.device, NULL));
    double probe_ns = (double)(ui_monotonic_ns() - probe_begin) / PROBES;
    uint32_t id;
    ui_hooks_subscribe(run.app, on_trace, NULL, &id);
    run.start = ui_monotonic_ns();
    run.end = run.start + WARMUP_NS + run.seconds * 1000000000ull;
    thrd_t thread;
    if (second_thread) thrd_create(&thread, !strcmp(mode, "burn") ? burn_thread : compile_thread, NULL);
    ui_app_run(run.app);
    atomic_store(&run.stop, true);
    if (second_thread) thrd_join(thread, NULL);
    ui_app_destroy(run.app);

    double hz = run.refresh_ns ? 1e9 / (double)run.refresh_ns : 0;
    printf("%s: %s, %llu s at %.1f Hz: %llu frames presented, %llu refreshes missed (%.2f%%)%s\n",
           label, mode, (unsigned long long)run.seconds, hz, (unsigned long long)run.frames,
           (unsigned long long)run.missed,
           100.0 * (double)run.missed / (double)(run.frames + run.missed + !(run.frames + run.missed)),
           atomic_load(&run.lost) ? " (device lost)" : "");

    printf("  device probe %.0f ns\n", probe_ns);

    FILE *out = json ? fopen(json, "a") : NULL;
    if (out)
        fprintf(out, "{\"label\":\"%s\",\"mode\":\"%s\",\"same\":%s,\"rows\":\"%s\",\"hz\":%.2f,\"seconds\":%llu,"
                     "\"frames\":%llu,\"missed\":%llu,\"lost\":%u,\"compile_errors\":%llu",
                label, mode, run.same ? "true" : "false", arg_count > 1 ? args[1] + 7 : "10000", hz,
                (unsigned long long)run.seconds, (unsigned long long)run.frames,
                (unsigned long long)run.missed, atomic_load(&run.lost), (unsigned long long)run.compile_errors);
    uint64_t *values = malloc(MAX_SAMPLES * sizeof *values);
    for (int i = 0; i < S_COUNT; i++) {
        uint32_t n = atomic_load(&run.series[i].count);
        for (uint32_t k = 0; k < n; k++) values[k] = run.series[i].samples[k].value;
        percentiles p = percentiles_of(values, n);
        printf("  %-9s n %6u  p50 %8.3f ms  p99 %8.3f ms  max %8.3f ms\n", series_names[i], p.n, p.p50, p.p99, p.max);
        if (out)
            fprintf(out, ",\"%s\":{\"n\":%u,\"p50\":%.4f,\"p99\":%.4f,\"max\":%.4f}", series_names[i], p.n, p.p50, p.p99, p.max);
    }
    if (second_thread) {
        for (int size = 0; size < SIZES; size++) {
            for (int kind = 0; kind < C_KINDS; kind++) {
                uint32_t n = run.compile_count[kind][size];
                if (!n) continue;
                percentiles p = percentiles_of(run.compile[kind][size], n);
                const char *name = size == LARGE ? "large" : "small";
                printf("  %s %-7s n %5u  p50 %8.3f ms  p99 %8.3f ms  max %8.3f ms\n", name, compile_names[kind], p.n, p.p50, p.p99, p.max);
                if (out)
                    fprintf(out, ",\"%s_%s\":{\"n\":%u,\"p50\":%.4f,\"p99\":%.4f,\"max\":%.4f}", name, compile_names[kind], p.n, p.p50, p.p99, p.max);
            }
        }
        if (run.fallback)
            printf("  after present: %llu shaders ran, %llu windows deferred, %llu forced past the budget\n",
                   (unsigned long long)after_present.ran, (unsigned long long)after_present.deferred,
                   (unsigned long long)after_present.forced);
        if (out && run.fallback)
            fprintf(out, ",\"ran\":%llu,\"deferred\":%llu,\"forced\":%llu", (unsigned long long)after_present.ran,
                    (unsigned long long)after_present.deferred, (unsigned long long)after_present.forced);
        if (run.compile_errors) printf("  %llu COMPILE ERRORS: this run measured nothing\n", (unsigned long long)run.compile_errors);
    }
    if (out) {
        fprintf(out, "}\n");
        fclose(out);
    }
    free(values);
    return run.compile_errors ? 1 : 0;
}
