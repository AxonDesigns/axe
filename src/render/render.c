// Render actor: owns the WebGPU instance, device, pipelines and per-surface targets.
// Every WebGPU call in the code base happens in this file, inside the render actor; in
// debug builds webgpu_checked.h turns each call into an assertion of that.
#include "render/render.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <threads.h>
#include <time.h>

#include "text/atlas.h"
#include "webgpu_checked.h"

#define MODULE "render"
#define INSTANCE_STRIDE 48u // sizeof(ui_dl_rect): rect, params, color
#define MAX_PIPELINES 4u

static_assert(sizeof(ui_dl_rect) == INSTANCE_STRIDE, "instance layout must match ui_dl_rect");

static const char rrect_wgsl[] = {
#embed "shaders/rrect.wgsl"
    , 0};
static const char glyph_wgsl[] = {
#embed "shaders/glyph.wgsl"
    , 0};

static_assert(sizeof(ui_dl_glyph) == INSTANCE_STRIDE, "glyph instances share the rect stride");

typedef struct render_surface {
    bool used;
    ui_handle handle;
    ui_native_kind kind;
    ui_mailbox *mailbox;
    ui_platform *platform;
    ui_psurface *psurface;
    WGPUSurface surface;           // windows
    WGPUTexture offscreen;         // headless
    WGPUTextureView offscreen_view;
    WGPUTextureFormat format;
    WGPUCompositeAlphaMode alpha_mode;
    uint32_t width, height;        // current target size
    bool configured, reconfigure;
    bool has_content;
    WGPUBuffer uniforms;
    WGPUBindGroup bind_group;
    WGPUBuffer instances;
    uint64_t instance_bytes;
    WGPUBuffer glyph_instances;
    uint64_t glyph_instance_bytes;
} render_surface;

// One atlas page texture. The core decides what goes where; this side only uploads.
typedef struct atlas_page {
    WGPUTexture texture;
    WGPUTextureView view;
    WGPUBindGroup bind_group;
    uint32_t size;
} atlas_page;

typedef struct pipeline_entry {
    WGPUTextureFormat format;
    WGPURenderPipeline rect;
    WGPURenderPipeline glyph;
} pipeline_entry;

struct ui_render {
    ui_actor actor;
    ui_render_desc desc;
    const ui_logger *log;
    WGPUInstance instance;
    WGPUAdapter adapter;
    WGPUDevice device;
    WGPUQueue queue;
    WGPUShaderModule shader;
    WGPUShaderModule glyph_shader;
    WGPUBindGroupLayout bind_group_layout;  // group 0: globals
    WGPUBindGroupLayout atlas_layout;       // group 1 of the glyph pipeline: one atlas page
    WGPUPipelineLayout pipeline_layout;
    WGPUPipelineLayout glyph_pipeline_layout;
    pipeline_entry pipelines[MAX_PIPELINES];
    uint32_t pipeline_count;
    render_surface surfaces[UI_MAX_SURFACES];
    atlas_page atlas[UI_ATLAS_MAX_PAGES];
    uint64_t atlas_seq; // newest atlas update applied
    // wgpu-native extensions, used only when available.
    struct {
        bool device_poll;
        bool max_frame_latency;
    } caps;
    _Atomic bool device_lost;
    bool failed;
};

// ---- Thread check ------------------------------------------------------------------------

#ifndef NDEBUG
// Depth of render-actor execution on this thread. WebGPU may only be called while > 0.
static thread_local int render_scope;
#define RENDER_SCOPE_ENTER() (render_scope++)
#define RENDER_SCOPE_EXIT() (render_scope--)

void ui_gpu_check_thread(const char *fn) {
    if (render_scope > 0) return;
    fprintf(stderr, "webgpu call %s outside the render actor\n", fn);
    abort();
}
#else
#define RENDER_SCOPE_ENTER() ((void)0)
#define RENDER_SCOPE_EXIT() ((void)0)
#endif

// ---- Helpers -----------------------------------------------------------------------------

static WGPUStringView sv(const char *s) { return (WGPUStringView){s, WGPU_STRLEN}; }

static void report_failure(ui_render *r, ui_result result) {
    if (r->failed) return;
    r->failed = true;
    ui_msg msg = {.type = UI_MSG_RENDER_FAILED, .u64 = (uint64_t)result};
    if (ui_queue_push(r->desc.core_queue, &msg) != UI_OK)
        UI_ERROR(r->log, MODULE, "core queue full; render failure not delivered");
}

static void on_device_lost(WGPUDevice const *device, WGPUDeviceLostReason reason,
                           WGPUStringView message, void *user1, void *user2) {
    (void)device;
    (void)user2;
    ui_render *r = user1;
    if (reason == WGPUDeviceLostReason_Destroyed || reason == WGPUDeviceLostReason_CallbackCancelled)
        return;
    UI_ERROR(r->log, MODULE, "device lost (%d): %.*s", (int)reason, (int)message.length,
             message.data);
    atomic_store(&r->device_lost, true);
}

static void on_uncaptured_error(WGPUDevice const *device, WGPUErrorType type,
                                WGPUStringView message, void *user1, void *user2) {
    (void)device;
    (void)user2;
    ui_render *r = user1;
    UI_ERROR(r->log, MODULE, "webgpu error (%d): %.*s", (int)type, (int)message.length,
             message.data);
}

typedef struct request_result {
    bool done;
    void *handle;
    char message[256]; // copied: the callback's string is only valid during the call
} request_result;

static void copy_message(request_result *res, WGPUStringView message) {
    size_t n = message.data ? (message.length == WGPU_STRLEN ? strlen(message.data) : message.length)
                            : 0;
    if (n >= sizeof res->message) n = sizeof res->message - 1;
    if (n) memcpy(res->message, message.data, n);
    res->message[n] = 0;
}

static void on_adapter(WGPURequestAdapterStatus status, WGPUAdapter adapter,
                       WGPUStringView message, void *user1, void *user2) {
    (void)user2;
    request_result *res = user1;
    res->done = true;
    res->handle = status == WGPURequestAdapterStatus_Success ? adapter : NULL;
    copy_message(res, message);
}

static void on_device(WGPURequestDeviceStatus status, WGPUDevice device, WGPUStringView message,
                      void *user1, void *user2) {
    (void)user2;
    request_result *res = user1;
    res->done = true;
    res->handle = status == WGPURequestDeviceStatus_Success ? device : NULL;
    copy_message(res, message);
}

// Waits for AllowProcessEvents callbacks. wgpu-native resolves requests immediately, but
// the standard API allows them to complete later.
static void wait_request(ui_render *r, const request_result *res) {
    for (int i = 0; i < 10000 && !res->done; i++) {
        wgpuInstanceProcessEvents(r->instance);
        if (!res->done) thrd_sleep(&(struct timespec){.tv_nsec = 100000}, NULL);
    }
}

// ---- Pipelines ---------------------------------------------------------------------------

static ui_result create_pipeline(ui_render *r, WGPUTextureFormat format, bool glyph,
                                 WGPURenderPipeline *out) {
    WGPUVertexAttribute attributes[3];
    for (uint32_t i = 0; i < 3; i++) {
        attributes[i] = WGPU_VERTEX_ATTRIBUTE_INIT;
        attributes[i].format = WGPUVertexFormat_Float32x4;
        attributes[i].offset = 16u * i;
        attributes[i].shaderLocation = i;
    }
    WGPUVertexBufferLayout instance_layout = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
    instance_layout.stepMode = WGPUVertexStepMode_Instance;
    instance_layout.arrayStride = INSTANCE_STRIDE;
    instance_layout.attributeCount = 3;
    instance_layout.attributes = attributes;

    WGPUBlendState blend = WGPU_BLEND_STATE_INIT;
    blend.color = (WGPUBlendComponent){WGPUBlendOperation_Add, WGPUBlendFactor_One,
                                       WGPUBlendFactor_OneMinusSrcAlpha};
    blend.alpha = blend.color;
    WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
    target.format = format;
    target.blend = &blend;
    target.writeMask = WGPUColorWriteMask_All;

    WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
    fragment.module = glyph ? r->glyph_shader : r->shader;
    fragment.entryPoint = sv("fs_main");
    fragment.targetCount = 1;
    fragment.targets = &target;

    WGPURenderPipelineDescriptor desc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    desc.label = sv(glyph ? "glyph" : "rrect");
    desc.layout = glyph ? r->glyph_pipeline_layout : r->pipeline_layout;
    desc.vertex.module = glyph ? r->glyph_shader : r->shader;
    desc.vertex.entryPoint = sv("vs_main");
    desc.vertex.bufferCount = 1;
    desc.vertex.buffers = &instance_layout;
    desc.primitive.topology = WGPUPrimitiveTopology_TriangleStrip;
    desc.fragment = &fragment;
    *out = wgpuDeviceCreateRenderPipeline(r->device, &desc);
    return *out ? UI_OK : UI_ERR_GPU;
}

static ui_result create_pipelines(ui_render *r, WGPUTextureFormat format, pipeline_entry *e) {
    *e = (pipeline_entry){.format = format};
    ui_result res = create_pipeline(r, format, false, &e->rect);
    if (res == UI_OK) res = create_pipeline(r, format, true, &e->glyph);
    return res;
}

static const pipeline_entry *pipelines_for(ui_render *r, WGPUTextureFormat format) {
    for (uint32_t i = 0; i < r->pipeline_count; i++) {
        if (r->pipelines[i].format == format) return &r->pipelines[i];
    }
    // Only reached for a surface format nobody precompiled; logged so it gets added.
    if (r->pipeline_count == MAX_PIPELINES) return NULL;
    UI_WARN(r->log, MODULE, "compiling pipelines for format %d at runtime", (int)format);
    pipeline_entry *e = &r->pipelines[r->pipeline_count];
    if (create_pipelines(r, format, e) != UI_OK) return NULL;
    r->pipeline_count++;
    return e;
}

// ---- Device ------------------------------------------------------------------------------

static ui_result init_device(ui_render *r) {
#ifdef UI_WEBGPU_WGPU_NATIVE
    r->caps.device_poll = true;
    r->caps.max_frame_latency = true;
#endif
    r->instance = wgpuCreateInstance(NULL);
    if (!r->instance) return UI_ERR_GPU;

    WGPURequestAdapterOptions options = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
    options.powerPreference = WGPUPowerPreference_HighPerformance;
    options.forceFallbackAdapter = r->desc.gpu == UI_GPU_SOFTWARE;
    request_result adapter = {0};
    WGPURequestAdapterCallbackInfo adapter_cb = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
    adapter_cb.mode = WGPUCallbackMode_AllowProcessEvents;
    adapter_cb.callback = on_adapter;
    adapter_cb.userdata1 = &adapter;
    wgpuInstanceRequestAdapter(r->instance, &options, adapter_cb);
    wait_request(r, &adapter);
    if (!adapter.handle) {
        UI_ERROR(r->log, MODULE, "no %s adapter (%s)%s",
                 r->desc.gpu == UI_GPU_SOFTWARE ? "software" : "GPU", adapter.message,
                 r->desc.gpu == UI_GPU_SOFTWARE
                     ? "; is a CPU Vulkan driver such as lavapipe installed and loadable? "
                       "Check `vulkaninfo --summary`"
                     : "");
        return UI_ERR_GPU;
    }
    r->adapter = adapter.handle;

    WGPUAdapterInfo info = WGPU_ADAPTER_INFO_INIT;
    if (wgpuAdapterGetInfo(r->adapter, &info) == WGPUStatus_Success) {
        UI_INFO(r->log, MODULE, "adapter: %.*s (%.*s)", (int)info.device.length, info.device.data,
                (int)info.description.length, info.description.data);
        wgpuAdapterInfoFreeMembers(info);
    }

    WGPUDeviceDescriptor device_desc = WGPU_DEVICE_DESCRIPTOR_INIT;
    device_desc.label = sv("axe");
    device_desc.deviceLostCallbackInfo.mode = WGPUCallbackMode_AllowSpontaneous;
    device_desc.deviceLostCallbackInfo.callback = on_device_lost;
    device_desc.deviceLostCallbackInfo.userdata1 = r;
    device_desc.uncapturedErrorCallbackInfo.callback = on_uncaptured_error;
    device_desc.uncapturedErrorCallbackInfo.userdata1 = r;
    request_result device = {0};
    WGPURequestDeviceCallbackInfo device_cb = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
    device_cb.mode = WGPUCallbackMode_AllowProcessEvents;
    device_cb.callback = on_device;
    device_cb.userdata1 = &device;
    wgpuAdapterRequestDevice(r->adapter, &device_desc, device_cb);
    wait_request(r, &device);
    if (!device.handle) {
        UI_ERROR(r->log, MODULE, "cannot create device: %s", device.message);
        return UI_ERR_GPU;
    }
    r->device = device.handle;
    r->queue = wgpuDeviceGetQueue(r->device);

    WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
    wgsl.code = sv(rrect_wgsl);
    WGPUShaderModuleDescriptor shader_desc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    shader_desc.nextInChain = &wgsl.chain;
    shader_desc.label = sv("rrect.wgsl");
    r->shader = wgpuDeviceCreateShaderModule(r->device, &shader_desc);
    wgsl.code = sv(glyph_wgsl);
    shader_desc.label = sv("glyph.wgsl");
    r->glyph_shader = wgpuDeviceCreateShaderModule(r->device, &shader_desc);
    if (!r->shader || !r->glyph_shader) return UI_ERR_GPU;

    WGPUBindGroupLayoutEntry entry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    entry.binding = 0;
    entry.visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
    entry.buffer.type = WGPUBufferBindingType_Uniform;
    entry.buffer.minBindingSize = 16;
    WGPUBindGroupLayoutDescriptor bgl_desc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    bgl_desc.entryCount = 1;
    bgl_desc.entries = &entry;
    r->bind_group_layout = wgpuDeviceCreateBindGroupLayout(r->device, &bgl_desc);
    WGPUPipelineLayoutDescriptor layout_desc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    layout_desc.bindGroupLayoutCount = 1;
    layout_desc.bindGroupLayouts = &r->bind_group_layout;
    r->pipeline_layout = wgpuDeviceCreatePipelineLayout(r->device, &layout_desc);

    WGPUBindGroupLayoutEntry tex = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    tex.binding = 0;
    tex.visibility = WGPUShaderStage_Fragment;
    tex.texture.sampleType = WGPUTextureSampleType_Float;
    tex.texture.viewDimension = WGPUTextureViewDimension_2D;
    WGPUBindGroupLayoutDescriptor tex_desc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    tex_desc.entryCount = 1;
    tex_desc.entries = &tex;
    r->atlas_layout = wgpuDeviceCreateBindGroupLayout(r->device, &tex_desc);
    WGPUBindGroupLayout glyph_layouts[2] = {r->bind_group_layout, r->atlas_layout};
    layout_desc.bindGroupLayoutCount = 2;
    layout_desc.bindGroupLayouts = glyph_layouts;
    r->glyph_pipeline_layout = wgpuDeviceCreatePipelineLayout(r->device, &layout_desc);
    if (!r->bind_group_layout || !r->pipeline_layout || !r->atlas_layout ||
        !r->glyph_pipeline_layout)
        return UI_ERR_GPU;

    // Precompile every variant used at runtime: windows (BGRA8 sRGB, the common swapchain
    // format) and offscreen targets (RGBA8 sRGB).
    static const WGPUTextureFormat formats[] = {WGPUTextureFormat_BGRA8UnormSrgb,
                                                WGPUTextureFormat_RGBA8UnormSrgb};
    for (size_t i = 0; i < UI_ARRAY_LEN(formats); i++) {
        ui_result res = create_pipelines(r, formats[i], &r->pipelines[r->pipeline_count]);
        if (res != UI_OK) return res;
        r->pipeline_count++;
    }
    return UI_OK;
}

// ---- Surfaces ----------------------------------------------------------------------------

static render_surface *find_surface(ui_render *r, ui_handle h) {
    for (uint32_t i = 0; i < UI_MAX_SURFACES; i++) {
        if (r->surfaces[i].used && ui_handle_eq(r->surfaces[i].handle, h)) return &r->surfaces[i];
    }
    return NULL;
}

static void release_surface(render_surface *s) {
    if (s->bind_group) wgpuBindGroupRelease(s->bind_group);
    if (s->uniforms) wgpuBufferRelease(s->uniforms);
    if (s->instances) wgpuBufferRelease(s->instances);
    if (s->glyph_instances) wgpuBufferRelease(s->glyph_instances);
    if (s->offscreen_view) wgpuTextureViewRelease(s->offscreen_view);
    if (s->offscreen) wgpuTextureRelease(s->offscreen);
    if (s->surface) {
        if (s->configured) wgpuSurfaceUnconfigure(s->surface);
        wgpuSurfaceRelease(s->surface);
    }
    memset(s, 0, sizeof *s);
}

static ui_result add_surface(ui_render *r, const ui_render_surface_desc *d) {
    render_surface *s = NULL;
    for (uint32_t i = 0; i < UI_MAX_SURFACES && !s; i++) {
        if (!r->surfaces[i].used) s = &r->surfaces[i];
    }
    if (!s) return UI_ERR_FULL;
    memset(s, 0, sizeof *s);
    s->used = true;
    s->handle = d->surface;
    s->kind = d->native.kind;
    s->mailbox = d->mailbox;
    s->platform = d->platform;
    s->psurface = d->psurface;

    WGPUBufferDescriptor ub = WGPU_BUFFER_DESCRIPTOR_INIT;
    ub.label = sv("globals");
    ub.size = 16;
    ub.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    s->uniforms = wgpuDeviceCreateBuffer(r->device, &ub);
    WGPUBindGroupEntry be = WGPU_BIND_GROUP_ENTRY_INIT;
    be.binding = 0;
    be.buffer = s->uniforms;
    be.size = 16;
    WGPUBindGroupDescriptor bg = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bg.layout = r->bind_group_layout;
    bg.entryCount = 1;
    bg.entries = &be;
    s->bind_group = wgpuDeviceCreateBindGroup(r->device, &bg);
    if (!s->uniforms || !s->bind_group) return UI_ERR_GPU;

    if (s->kind == UI_NATIVE_OFFSCREEN) {
        s->format = WGPUTextureFormat_RGBA8UnormSrgb;
        return UI_OK;
    }

    WGPUSurfaceSourceWaylandSurface source = WGPU_SURFACE_SOURCE_WAYLAND_SURFACE_INIT;
    source.display = d->native.display;
    source.surface = d->native.surface;
    WGPUSurfaceDescriptor sd = WGPU_SURFACE_DESCRIPTOR_INIT;
    sd.nextInChain = &source.chain;
    sd.label = sv("window");
    s->surface = wgpuInstanceCreateSurface(r->instance, &sd);
    if (!s->surface) return UI_ERR_GPU;

    WGPUSurfaceCapabilities caps = WGPU_SURFACE_CAPABILITIES_INIT;
    if (wgpuSurfaceGetCapabilities(s->surface, r->adapter, &caps) != WGPUStatus_Success ||
        !caps.formatCount) {
        UI_ERROR(r->log, MODULE, "the adapter cannot present to this surface");
        return UI_ERR_GPU;
    }
    // Prefer an sRGB format so blending happens in linear light and the swapchain encodes.
    s->format = WGPUTextureFormat_Undefined;
    for (size_t i = 0; i < caps.formatCount && !s->format; i++) {
        if (caps.formats[i] == WGPUTextureFormat_BGRA8UnormSrgb ||
            caps.formats[i] == WGPUTextureFormat_RGBA8UnormSrgb)
            s->format = caps.formats[i];
    }
    if (!s->format) {
        s->format = caps.formats[0];
        UI_WARN(r->log, MODULE, "no sRGB swapchain format; colors will be too dark");
    }
    s->alpha_mode = caps.alphaModes[0];
    for (size_t i = 0; i < caps.alphaModeCount; i++) {
        if (caps.alphaModes[i] == WGPUCompositeAlphaMode_Opaque)
            s->alpha_mode = WGPUCompositeAlphaMode_Opaque;
    }
    wgpuSurfaceCapabilitiesFreeMembers(caps);
    return UI_OK;
}

static void configure_window(ui_render *r, render_surface *s, uint32_t width, uint32_t height) {
    WGPUSurfaceConfiguration config = WGPU_SURFACE_CONFIGURATION_INIT;
    config.device = r->device;
    config.format = s->format;
    config.usage = WGPUTextureUsage_RenderAttachment;
    config.width = width;
    config.height = height;
    config.alphaMode = s->alpha_mode;
    config.presentMode = WGPUPresentMode_Fifo;
#ifdef UI_WEBGPU_WGPU_NATIVE
    // At most one frame queued between us and the display.
    WGPUSurfaceConfigurationExtras extras = {
        .chain = {.sType = (WGPUSType)WGPUSType_SurfaceConfigurationExtras},
        .desiredMaximumFrameLatency = 1,
    };
    if (r->caps.max_frame_latency) config.nextInChain = &extras.chain;
#endif
    wgpuSurfaceConfigure(s->surface, &config);
    s->width = width;
    s->height = height;
    s->configured = true;
    s->reconfigure = false;
    UI_DEBUG(r->log, MODULE, "swapchain %ux%u", width, height);
}

static ui_result ensure_offscreen(ui_render *r, render_surface *s, uint32_t width, uint32_t height) {
    if (s->offscreen && s->width == width && s->height == height) return UI_OK;
    if (s->offscreen_view) wgpuTextureViewRelease(s->offscreen_view);
    if (s->offscreen) wgpuTextureRelease(s->offscreen);
    WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
    td.label = sv("headless");
    td.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
    td.size = (WGPUExtent3D){width, height, 1};
    td.format = s->format;
    s->offscreen = wgpuDeviceCreateTexture(r->device, &td);
    s->offscreen_view = s->offscreen ? wgpuTextureCreateView(s->offscreen, NULL) : NULL;
    s->width = width;
    s->height = height;
    s->has_content = false;
    return s->offscreen_view ? UI_OK : UI_ERR_GPU;
}

// ---- Atlas -------------------------------------------------------------------------------

static void release_page(atlas_page *p) {
    if (p->bind_group) wgpuBindGroupRelease(p->bind_group);
    if (p->view) wgpuTextureViewRelease(p->view);
    if (p->texture) {
        wgpuTextureDestroy(p->texture);
        wgpuTextureRelease(p->texture);
    }
    *p = (atlas_page){0};
}

static ui_result create_page(ui_render *r, atlas_page *p, uint32_t size, uint16_t format) {
    release_page(p);
    WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
    td.label = sv("glyph atlas");
    td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    td.size = (WGPUExtent3D){size, size, 1};
    td.format = format == UI_ATLAS_RGBA8 ? WGPUTextureFormat_RGBA8Unorm : WGPUTextureFormat_R8Unorm;
    p->texture = wgpuDeviceCreateTexture(r->device, &td);
    p->view = p->texture ? wgpuTextureCreateView(p->texture, NULL) : NULL;
    p->size = size;
    if (!p->view) return UI_ERR_GPU;
    WGPUBindGroupEntry be = WGPU_BIND_GROUP_ENTRY_INIT;
    be.binding = 0;
    be.textureView = p->view;
    WGPUBindGroupDescriptor bg = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bg.layout = r->atlas_layout;
    bg.entryCount = 1;
    bg.entries = &be;
    p->bind_group = wgpuDeviceCreateBindGroup(r->device, &bg);
    if (!p->bind_group) return UI_ERR_GPU;
    // New pages start transparent (WebGPU zero-initializes, but say so explicitly: released
    // and recreated slots must never show stale glyphs).
    uint32_t bpp = format == UI_ATLAS_RGBA8 ? 4 : 1;
    uint8_t *zeros = calloc((size_t)size * size, bpp);
    if (!zeros) return UI_ERR_OUT_OF_MEMORY;
    WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    dst.texture = p->texture;
    WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
    layout.bytesPerRow = size * bpp;
    layout.rowsPerImage = size;
    wgpuQueueWriteTexture(r->queue, &dst, zeros, (size_t)size * size * bpp, &layout,
                          &(WGPUExtent3D){size, size, 1});
    free(zeros);
    return UI_OK;
}

// Applies one update: page creations and releases, then texel writes, in order. Runs only
// between display lists (see the protocol in text/atlas.h), so a write is always queued
// after every draw that was submitted before it.
static ui_result apply_atlas_update(ui_render *r, const ui_atlas_update *u) {
    for (uint32_t i = 0; i < u->page_op_count; i++) {
        const ui_atlas_page_op *op = &u->page_ops[i];
        if (op->page >= UI_ATLAS_MAX_PAGES) return UI_ERR_INVALID_ARGUMENT;
        if (!op->size) {
            release_page(&r->atlas[op->page]);
        } else {
            ui_result res = create_page(r, &r->atlas[op->page], op->size, op->format);
            if (res != UI_OK) return res;
        }
    }
    for (uint32_t i = 0; i < u->write_count; i++) {
        const ui_atlas_write *w = &u->writes[i];
        atlas_page *p = w->page < UI_ATLAS_MAX_PAGES ? &r->atlas[w->page] : NULL;
        if (!p || !p->texture || w->x + w->width > p->size || w->y + w->height > p->size)
            return UI_ERR_INVALID_ARGUMENT;
        WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
        dst.texture = p->texture;
        dst.origin = (WGPUOrigin3D){w->x, w->y, 0};
        WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
        layout.bytesPerRow = w->width;
        layout.rowsPerImage = w->height;
        wgpuQueueWriteTexture(r->queue, &dst, u->pixels + w->offset,
                              (size_t)w->width * w->height, &layout,
                              &(WGPUExtent3D){w->width, w->height, 1});
    }
    r->atlas_seq = u->seq;
    return UI_OK;
}

// ---- Frames ------------------------------------------------------------------------------

// Grows a per-surface instance buffer to hold `bytes` and uploads `data` into it.
static ui_result upload_instances(ui_render *r, WGPUBuffer *buffer, uint64_t *capacity,
                                  const void *data, uint64_t bytes, const char *label) {
    if (!bytes) return UI_OK;
    if (bytes > *capacity) {
        uint64_t size = *capacity ? *capacity : 4096;
        while (size < bytes) size *= 2;
        if (*buffer) wgpuBufferRelease(*buffer);
        WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
        bd.label = sv(label);
        bd.size = size;
        bd.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
        *buffer = wgpuDeviceCreateBuffer(r->device, &bd);
        *capacity = *buffer ? size : 0;
        if (!*buffer) return UI_ERR_GPU;
    }
    wgpuQueueWriteBuffer(r->queue, *buffer, 0, data, (size_t)bytes);
    return UI_OK;
}

static ui_result encode(ui_render *r, render_surface *s, const ui_display_list *dl,
                        WGPUTextureView target) {
    const pipeline_entry *pipes = pipelines_for(r, s->format);
    if (!pipes) return UI_ERR_GPU;
    const float globals[4] = {(float)dl->width, (float)dl->height, dl->text_contrast,
                              dl->text_gamma};
    wgpuQueueWriteBuffer(r->queue, s->uniforms, 0, globals, sizeof globals);

    uint64_t rect_bytes = (uint64_t)dl->rect_count * INSTANCE_STRIDE;
    uint64_t glyph_bytes = (uint64_t)dl->glyph_count * INSTANCE_STRIDE;
    ui_result res = upload_instances(r, &s->instances, &s->instance_bytes, dl->rects, rect_bytes,
                                     "rect instances");
    if (res == UI_OK)
        res = upload_instances(r, &s->glyph_instances, &s->glyph_instance_bytes, dl->glyphs,
                               glyph_bytes, "glyph instances");
    if (res != UI_OK) return res;

    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(r->device, NULL);
    WGPURenderPassColorAttachment color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    color.view = target;
    color.loadOp = WGPULoadOp_Clear;
    color.storeOp = WGPUStoreOp_Store;
    color.clearValue = (WGPUColor){dl->clear[0], dl->clear[1], dl->clear[2], dl->clear[3]};
    WGPURenderPassDescriptor pass_desc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    pass_desc.colorAttachmentCount = 1;
    pass_desc.colorAttachments = &color;
    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, &pass_desc);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, s->bind_group, 0, NULL);
    int bound = -1; // which pipeline is set: UI_DL_RECTS or UI_DL_GLYPHS
    int32_t page = -1;
    uint32_t scissor[4] = {0, 0, dl->width, dl->height}; // the pass starts unclipped
    for (uint32_t i = 0; i < dl->batch_count; i++) {
        const ui_dl_batch *b = &dl->batches[i];
        // The batch's clip, clamped to the target (a scissor must lie inside it).
        int64_t cx0 = b->clip.x, cy0 = b->clip.y;
        int64_t cx1 = cx0 + b->clip.width, cy1 = cy0 + b->clip.height;
        uint32_t x0 = (uint32_t)(cx0 < 0 ? 0 : cx0 > dl->width ? dl->width : cx0);
        uint32_t y0 = (uint32_t)(cy0 < 0 ? 0 : cy0 > dl->height ? dl->height : cy0);
        uint32_t x1 = (uint32_t)(cx1 < x0 ? x0 : cx1 > dl->width ? dl->width : cx1);
        uint32_t y1 = (uint32_t)(cy1 < y0 ? y0 : cy1 > dl->height ? dl->height : cy1);
        if (x1 == x0 || y1 == y0) continue; // clipped away entirely
        if (x0 != scissor[0] || y0 != scissor[1] || x1 - x0 != scissor[2] || y1 - y0 != scissor[3]) {
            wgpuRenderPassEncoderSetScissorRect(pass, x0, y0, x1 - x0, y1 - y0);
            scissor[0] = x0;
            scissor[1] = y0;
            scissor[2] = x1 - x0;
            scissor[3] = y1 - y0;
        }
        if (b->kind == UI_DL_GLYPHS) {
            const atlas_page *ap = b->page < UI_ATLAS_MAX_PAGES ? &r->atlas[b->page] : NULL;
            if (!ap || !ap->bind_group) continue; // never sent: nothing to draw from
            if (bound != UI_DL_GLYPHS) {
                wgpuRenderPassEncoderSetPipeline(pass, pipes->glyph);
                wgpuRenderPassEncoderSetVertexBuffer(pass, 0, s->glyph_instances, 0, glyph_bytes);
                bound = UI_DL_GLYPHS;
                page = -1;
            }
            if (page != b->page) {
                wgpuRenderPassEncoderSetBindGroup(pass, 1, ap->bind_group, 0, NULL);
                page = b->page;
            }
        } else if (bound != UI_DL_RECTS) {
            wgpuRenderPassEncoderSetPipeline(pass, pipes->rect);
            wgpuRenderPassEncoderSetVertexBuffer(pass, 0, s->instances, 0, rect_bytes);
            bound = UI_DL_RECTS;
        }
        wgpuRenderPassEncoderDraw(pass, 4, b->count, 0, b->first);
    }
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);
    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, NULL);
    wgpuQueueSubmit(r->queue, 1, &cmd);
    wgpuCommandBufferRelease(cmd);
    wgpuCommandEncoderRelease(enc);
    return UI_OK;
}

// Draws one display list. Returns false when the frame was skipped (target unavailable).
static bool draw_window(ui_render *r, render_surface *s, const ui_display_list *dl) {
    UI_HOOK_PHASE_BEGIN(r->desc.hooks, UI_PHASE_ENCODE, s->handle, dl->frame_id);
    WGPUSurfaceTexture st = WGPU_SURFACE_TEXTURE_INIT;
    // A size change reconfigures before acquiring, so the frame that carries the new size
    // is rendered at it: no stretched frame. One retry covers an outdated swapchain.
    for (int attempt = 0; attempt < 2; attempt++) {
        if (!s->configured || s->reconfigure || s->width != dl->width || s->height != dl->height)
            configure_window(r, s, dl->width, dl->height);
        st = WGPU_SURFACE_TEXTURE_INIT;
        wgpuSurfaceGetCurrentTexture(s->surface, &st);
        if (st.status == WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal ||
            st.status == WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal)
            break;
        if (st.texture) wgpuTextureRelease(st.texture);
        st.texture = NULL;
        if (st.status == WGPUSurfaceGetCurrentTextureStatus_Error) break;
        s->reconfigure = true; // Timeout, Outdated, Lost
    }
    if (!st.texture) {
        UI_HOOK_PHASE_END(r->desc.hooks, UI_PHASE_ENCODE, s->handle, dl->frame_id);
        UI_WARN(r->log, MODULE, "skipped frame %" PRIu64 ": no swapchain image (status %d)",
                dl->frame_id, (int)st.status);
        return false;
    }
    if (st.status == WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) s->reconfigure = true;
    WGPUTextureView view = wgpuTextureCreateView(st.texture, NULL);
    ui_result res = view ? encode(r, s, dl, view) : UI_ERR_GPU;
    UI_HOOK_PHASE_END(r->desc.hooks, UI_PHASE_ENCODE, s->handle, dl->frame_id);

    UI_HOOK_PHASE_BEGIN(r->desc.hooks, UI_PHASE_PRESENT, s->handle, dl->frame_id);
    ui_present_info info = {
        .token = dl->platform_token,
        .frame_id = dl->frame_id,
        .predicted_present_host_ns = dl->predicted_present_host_ns,
        .logical_width = dl->logical_width,
        .logical_height = dl->logical_height,
        .scale_120 = dl->scale_120,
    };
    s->platform->vt->surface_pre_present(s->psurface, &info);
    wgpuSurfacePresent(s->surface);
    UI_HOOK_PHASE_END(r->desc.hooks, UI_PHASE_PRESENT, s->handle, dl->frame_id);
    if (view) wgpuTextureViewRelease(view);
    wgpuTextureRelease(st.texture);
    if (res != UI_OK) report_failure(r, res);
    return true;
}

static void draw_offscreen(ui_render *r, render_surface *s, const ui_display_list *dl) {
    UI_HOOK_PHASE_BEGIN(r->desc.hooks, UI_PHASE_ENCODE, s->handle, dl->frame_id);
    ui_result res = ensure_offscreen(r, s, dl->width, dl->height);
    if (res == UI_OK) res = encode(r, s, dl, s->offscreen_view);
    if (res == UI_OK) s->has_content = true;
    UI_HOOK_PHASE_END(r->desc.hooks, UI_PHASE_ENCODE, s->handle, dl->frame_id);
    if (res != UI_OK) report_failure(r, res);
}

static void frame_done(ui_render *r, render_surface *s, uint64_t frame_id, bool skipped) {
    UI_HOOK(r->desc.hooks, UI_TRACE_FRAME_END, 0, s->handle, frame_id);
    ui_msg msg = {.type = UI_MSG_FRAME_DONE, .surface = s->handle, .u64 = frame_id,
                  .size = skipped ? 1 : 0};
    if (ui_queue_push(r->desc.core_queue, &msg) != UI_OK)
        UI_ERROR(r->log, MODULE, "core queue full; lost frame-done for %" PRIu64, frame_id);
}

// ---- Capture -----------------------------------------------------------------------------

typedef struct map_result {
    bool done;
    WGPUMapAsyncStatus status;
} map_result;

static void on_mapped(WGPUMapAsyncStatus status, WGPUStringView message, void *user1,
                      void *user2) {
    (void)message;
    (void)user2;
    map_result *m = user1;
    m->done = true;
    m->status = status;
}

static ui_result capture(ui_render *r, render_surface *s, ui_image *image) {
    if (s->kind != UI_NATIVE_OFFSCREEN) return UI_ERR_UNSUPPORTED;
    if (!s->has_content) return UI_ERR_WRONG_STATE;
    uint32_t bpr = (s->width * 4 + 255) & ~255u;
    uint64_t size = (uint64_t)bpr * s->height;
    WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
    bd.label = sv("readback");
    bd.size = size;
    bd.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
    WGPUBuffer buffer = wgpuDeviceCreateBuffer(r->device, &bd);
    if (!buffer) return UI_ERR_GPU;

    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(r->device, NULL);
    WGPUTexelCopyTextureInfo src = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    src.texture = s->offscreen;
    WGPUTexelCopyBufferInfo dst = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
    dst.buffer = buffer;
    dst.layout.bytesPerRow = bpr;
    dst.layout.rowsPerImage = s->height;
    WGPUExtent3D extent = {s->width, s->height, 1};
    wgpuCommandEncoderCopyTextureToBuffer(enc, &src, &dst, &extent);
    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, NULL);
    wgpuQueueSubmit(r->queue, 1, &cmd);
    wgpuCommandBufferRelease(cmd);
    wgpuCommandEncoderRelease(enc);

    map_result mapped = {0};
    WGPUBufferMapCallbackInfo cb = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
    cb.mode = WGPUCallbackMode_AllowProcessEvents;
    cb.callback = on_mapped;
    cb.userdata1 = &mapped;
    wgpuBufferMapAsync(buffer, WGPUMapMode_Read, 0, (size_t)size, cb);
    for (int i = 0; i < 100000 && !mapped.done; i++) {
#ifdef UI_WEBGPU_WGPU_NATIVE
        if (r->caps.device_poll) wgpuDevicePoll(r->device, true, NULL);
#endif
        wgpuInstanceProcessEvents(r->instance);
        if (!mapped.done) thrd_sleep(&(struct timespec){.tv_nsec = 100000}, NULL);
    }
    ui_result res = UI_ERR_GPU;
    if (mapped.done && mapped.status == WGPUMapAsyncStatus_Success) {
        const uint8_t *data = wgpuBufferGetConstMappedRange(buffer, 0, (size_t)size);
        image->width = s->width;
        image->height = s->height;
        image->stride = s->width * 4;
        image->pixels = data ? malloc((size_t)image->stride * image->height) : NULL;
        if (image->pixels) {
            for (uint32_t y = 0; y < s->height; y++)
                memcpy(image->pixels + (size_t)y * image->stride, data + (size_t)y * bpr,
                       image->stride);
            res = UI_OK;
        } else {
            res = data ? UI_ERR_OUT_OF_MEMORY : UI_ERR_GPU;
        }
        wgpuBufferUnmap(buffer);
    }
    wgpuBufferRelease(buffer);
    return res;
}

// ---- Actor -------------------------------------------------------------------------------

static void release_all(ui_render *r);

static ui_result render_start(void *self) {
    ui_render *r = self;
    RENDER_SCOPE_ENTER();
    ui_result res = init_device(r);
    if (res != UI_OK) release_all(r); // stop() is not called after a failed start
    RENDER_SCOPE_EXIT();
    return res;
}

static void handle_message(ui_render *r, ui_msg *msg) {
    switch (msg->type) {
    case UI_MSG_SURFACE_ADD: {
        ui_result res = add_surface(r, msg->ptr);
        if (res != UI_OK) {
            UI_ERROR(r->log, MODULE, "cannot add surface: %s", ui_result_str(res));
            report_failure(r, res);
        }
        break;
    }
    case UI_MSG_SURFACE_REMOVE: {
        render_surface *s = find_surface(r, msg->surface);
        if (s) release_surface(s);
        break;
    }
    case UI_MSG_CAPTURE: {
        ui_capture_request *req = msg->ptr;
        render_surface *s = find_surface(r, req->surface);
        req->result = s ? capture(r, s, req->image) : UI_ERR_STALE_HANDLE;
        atomic_store(&req->done, true);
        ui_signal_notify(r->desc.progress);
        break;
    }
    case UI_MSG_ATLAS_UPDATE: {
        ui_result res = apply_atlas_update(r, msg->ptr);
        if (res != UI_OK) {
            UI_ERROR(r->log, MODULE, "cannot apply atlas update: %s", ui_result_str(res));
            report_failure(r, res);
        }
        break;
    }
    default:
        break;
    }
    ui_msg_drop(msg);
}

static void render_step(void *self) {
    ui_render *r = self;
    RENDER_SCOPE_ENTER();
    ui_msg msg;
    while (ui_queue_pop(&r->actor.queue, &msg)) handle_message(r, &msg);

    if (atomic_load(&r->device_lost)) report_failure(r, UI_ERR_GPU);
    for (uint32_t i = 0; i < UI_MAX_SURFACES && !r->failed; i++) {
        render_surface *s = &r->surfaces[i];
        if (!s->used) continue;
        const ui_display_list *dl = ui_mailbox_take(s->mailbox);
        if (!dl) continue;
        // The core pushed this list's atlas updates before publishing it, so they are in the
        // inbox now; apply them (and anything queued before them) first.
        while (r->atlas_seq < dl->atlas_seq && ui_queue_pop(&r->actor.queue, &msg))
            handle_message(r, &msg);
        if (r->atlas_seq < dl->atlas_seq)
            UI_ERROR(r->log, MODULE, "frame %" PRIu64 " needs atlas update %" PRIu64 ", have %" PRIu64,
                     dl->frame_id, dl->atlas_seq, r->atlas_seq);
        bool drawn = true;
        if (s->kind == UI_NATIVE_OFFSCREEN) draw_offscreen(r, s, dl);
        else drawn = draw_window(r, s, dl);
        frame_done(r, s, dl->frame_id, !drawn);
    }
    RENDER_SCOPE_EXIT();
}

static void release_all(ui_render *r) {
    for (uint32_t i = 0; i < UI_MAX_SURFACES; i++) {
        if (r->surfaces[i].used) release_surface(&r->surfaces[i]);
    }
    for (uint32_t i = 0; i < UI_ATLAS_MAX_PAGES; i++) release_page(&r->atlas[i]);
    for (uint32_t i = 0; i < r->pipeline_count; i++) {
        if (r->pipelines[i].rect) wgpuRenderPipelineRelease(r->pipelines[i].rect);
        if (r->pipelines[i].glyph) wgpuRenderPipelineRelease(r->pipelines[i].glyph);
    }
    r->pipeline_count = 0;
    if (r->pipeline_layout) wgpuPipelineLayoutRelease(r->pipeline_layout);
    if (r->glyph_pipeline_layout) wgpuPipelineLayoutRelease(r->glyph_pipeline_layout);
    if (r->bind_group_layout) wgpuBindGroupLayoutRelease(r->bind_group_layout);
    if (r->atlas_layout) wgpuBindGroupLayoutRelease(r->atlas_layout);
    if (r->shader) wgpuShaderModuleRelease(r->shader);
    if (r->glyph_shader) wgpuShaderModuleRelease(r->glyph_shader);
    if (r->queue) wgpuQueueRelease(r->queue);
    if (r->device) {
        wgpuDeviceDestroy(r->device);
        wgpuDeviceRelease(r->device);
    }
    if (r->adapter) wgpuAdapterRelease(r->adapter);
    if (r->instance) wgpuInstanceRelease(r->instance);
    r->pipeline_layout = NULL;
    r->glyph_pipeline_layout = NULL;
    r->bind_group_layout = NULL;
    r->atlas_layout = NULL;
    r->shader = NULL;
    r->glyph_shader = NULL;
    r->queue = NULL;
    r->device = NULL;
    r->adapter = NULL;
    r->instance = NULL;
}

static void render_stop(void *self) {
    RENDER_SCOPE_ENTER();
    release_all(self);
    RENDER_SCOPE_EXIT();
}

ui_result ui_render_create(const ui_render_desc *desc, ui_render **out) {
    ui_render *r = calloc(1, sizeof *r);
    if (!r) return UI_ERR_OUT_OF_MEMORY;
    r->desc = *desc;
    r->log = desc->log;
    atomic_init(&r->device_lost, false);
    ui_result res = ui_actor_init(&r->actor, "render", r, render_start, render_step, render_stop,
                                  UI_QUEUE_UNBOUNDED, (ui_waker){0});
    if (res != UI_OK) {
        free(r);
        return res;
    }
    *out = r;
    return UI_OK;
}

void ui_render_destroy(ui_render *r) {
    if (!r) return;
    ui_actor_destroy(&r->actor);
    free(r);
}

ui_actor *ui_render_actor(ui_render *r) { return &r->actor; }
