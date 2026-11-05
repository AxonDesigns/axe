// A Vulkan layer that fakes a lost device: after some presents, one chosen kind of call returns
// VK_ERROR_DEVICE_LOST, and from then on every call that can return it does, as on a real loss.
// It's how to see what wgpu does when the driver itself says so, which ui_render_test_lose_device
// can't show, and it risks no real GPU hang. See tools/vk-lost-layer/README.md.
//
// There are no Vulkan headers among the project's dependencies, so this declares the little it
// needs. Every type below is part of Vulkan's stable ABI.
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int32_t VkResult;
typedef void *Handle;
#define VK_SUCCESS 0
#define VK_ERROR_DEVICE_LOST (-4)
#define LOADER_INSTANCE_CREATE_INFO 47
#define LOADER_DEVICE_CREATE_INFO 48
#define VK_LAYER_LINK_INFO 0
#define EXPORT __attribute__((visibility("default")))

typedef void (*Proc)(void);
typedef Proc (*GetProc)(Handle, const char *);

typedef struct InstanceLink {
    struct InstanceLink *next;
    GetProc get_instance_proc;
    Proc get_physical_device_proc;
} InstanceLink;

typedef struct DeviceLink {
    struct DeviceLink *next;
    GetProc get_instance_proc;
    GetProc get_device_proc;
} DeviceLink;

typedef struct LayerCreateInfo {
    int32_t type;
    const void *next;
    int32_t function;
    void *link; // InstanceLink * or DeviceLink *, the first member of a union
} LayerCreateInfo;

typedef struct Chained {
    int32_t type;
    const void *next;
} Chained;

static LayerCreateInfo *find_link(const void *create_info, int32_t type) {
    for (const Chained *c = ((const Chained *)create_info)->next; c; c = c->next) {
        if (c->type != type) continue;
        LayerCreateInfo *info = (LayerCreateInfo *)c;
        if (info->function == VK_LAYER_LINK_INFO) return info;
    }
    return NULL;
}

// ---- When to fail ------------------------------------------------------------------------

enum { SITE_NONE, SITE_SUBMIT, SITE_ACQUIRE, SITE_PRESENT, SITE_POLL };
static int fail_site = SITE_NONE;
static long after_presents = 120;
static atomic_long presents;
static atomic_bool lost;

static void read_settings(void) {
    const char *site = getenv("VK_LOST_SITE"), *after = getenv("VK_LOST_AFTER");
    if (after) after_presents = atol(after);
    if (!site) return;
    fail_site = !strcmp(site, "submit") ? SITE_SUBMIT : !strcmp(site, "acquire") ? SITE_ACQUIRE
              : !strcmp(site, "present") ? SITE_PRESENT : !strcmp(site, "poll") ? SITE_POLL : SITE_NONE;
}

static bool fails(int site, const char *name) {
    if (atomic_load(&lost)) return true;
    if (site != fail_site || atomic_load(&presents) < after_presents) return false;
    fprintf(stderr, "[vk-lost-layer] %s returns VK_ERROR_DEVICE_LOST\n", name);
    atomic_store(&lost, true);
    return true;
}

// ---- The calls ---------------------------------------------------------------------------

static GetProc next_get_instance_proc, next_get_device_proc;
static Handle the_instance;

#define NEXT(ret, name, ...) static ret(*next_##name)(__VA_ARGS__)
NEXT(VkResult, vkQueueSubmit, Handle, uint32_t, const void *, Handle);
NEXT(VkResult, vkQueueSubmit2, Handle, uint32_t, const void *, Handle);
NEXT(VkResult, vkQueuePresentKHR, Handle, const void *);
NEXT(VkResult, vkAcquireNextImageKHR, Handle, Handle, uint64_t, Handle, Handle, uint32_t *);
NEXT(VkResult, vkGetSemaphoreCounterValue, Handle, Handle, uint64_t *);
NEXT(VkResult, vkGetFenceStatus, Handle, Handle);
NEXT(VkResult, vkWaitSemaphores, Handle, const void *, uint64_t);
NEXT(VkResult, vkWaitForFences, Handle, uint32_t, const Handle *, uint32_t, uint64_t);
NEXT(VkResult, vkDeviceWaitIdle, Handle);
NEXT(VkResult, vkQueueWaitIdle, Handle);

static VkResult vkQueueSubmit(Handle q, uint32_t n, const void *s, Handle f) {
    return fails(SITE_SUBMIT, "vkQueueSubmit") ? VK_ERROR_DEVICE_LOST : next_vkQueueSubmit(q, n, s, f);
}
static VkResult vkQueueSubmit2(Handle q, uint32_t n, const void *s, Handle f) {
    return fails(SITE_SUBMIT, "vkQueueSubmit2") ? VK_ERROR_DEVICE_LOST : next_vkQueueSubmit2(q, n, s, f);
}
static VkResult vkQueuePresentKHR(Handle q, const void *info) {
    atomic_fetch_add(&presents, 1);
    return fails(SITE_PRESENT, "vkQueuePresentKHR") ? VK_ERROR_DEVICE_LOST : next_vkQueuePresentKHR(q, info);
}
static VkResult vkAcquireNextImageKHR(Handle d, Handle c, uint64_t t, Handle s, Handle f, uint32_t *i) {
    return fails(SITE_ACQUIRE, "vkAcquireNextImageKHR") ? VK_ERROR_DEVICE_LOST
                                                        : next_vkAcquireNextImageKHR(d, c, t, s, f, i);
}
static VkResult vkGetSemaphoreCounterValue(Handle d, Handle s, uint64_t *v) {
    return fails(SITE_POLL, "vkGetSemaphoreCounterValue") ? VK_ERROR_DEVICE_LOST
                                                          : next_vkGetSemaphoreCounterValue(d, s, v);
}
static VkResult vkGetFenceStatus(Handle d, Handle f) {
    return fails(SITE_POLL, "vkGetFenceStatus") ? VK_ERROR_DEVICE_LOST : next_vkGetFenceStatus(d, f);
}
// Only ever after the loss: these are what shutting down waits on.
static VkResult vkWaitSemaphores(Handle d, const void *info, uint64_t t) {
    return atomic_load(&lost) ? VK_ERROR_DEVICE_LOST : next_vkWaitSemaphores(d, info, t);
}
static VkResult vkWaitForFences(Handle d, uint32_t n, const Handle *f, uint32_t all, uint64_t t) {
    return atomic_load(&lost) ? VK_ERROR_DEVICE_LOST : next_vkWaitForFences(d, n, f, all, t);
}
static VkResult vkDeviceWaitIdle(Handle d) {
    return atomic_load(&lost) ? VK_ERROR_DEVICE_LOST : next_vkDeviceWaitIdle(d);
}
static VkResult vkQueueWaitIdle(Handle q) {
    return atomic_load(&lost) ? VK_ERROR_DEVICE_LOST : next_vkQueueWaitIdle(q);
}

// Calls that aren't available stay NULL, and so does what we return for them.
#define WRAP(name)                                                                             \
    if (!strcmp(requested, #name) || !strcmp(requested, #name "KHR"))                          \
        return next_##name ? (Proc)name : NULL

static Proc wrapped(const char *requested) {
    WRAP(vkQueueSubmit);
    WRAP(vkQueueSubmit2);
    WRAP(vkQueuePresentKHR);
    WRAP(vkAcquireNextImageKHR);
    WRAP(vkGetSemaphoreCounterValue);
    WRAP(vkGetFenceStatus);
    WRAP(vkWaitSemaphores);
    WRAP(vkWaitForFences);
    WRAP(vkDeviceWaitIdle);
    WRAP(vkQueueWaitIdle);
    return NULL;
}

// ---- The layer ---------------------------------------------------------------------------

static Proc get_device_proc(Handle device, const char *name);
static Proc get_instance_proc(Handle instance, const char *name);

static VkResult create_instance(const void *info, const void *allocator, Handle *out) {
    read_settings();
    LayerCreateInfo *link = find_link(info, LOADER_INSTANCE_CREATE_INFO);
    if (!link) return -3; // VK_ERROR_INITIALIZATION_FAILED
    InstanceLink *chain = link->link;
    next_get_instance_proc = chain->get_instance_proc;
    link->link = chain->next;
    VkResult (*create)(const void *, const void *, Handle *) =
        (VkResult(*)(const void *, const void *, Handle *))next_get_instance_proc(NULL, "vkCreateInstance");
    VkResult r = create(info, allocator, out);
    if (r == VK_SUCCESS) the_instance = *out;
    return r;
}

static VkResult create_device(Handle physical, const void *info, const void *allocator, Handle *out) {
    LayerCreateInfo *link = find_link(info, LOADER_DEVICE_CREATE_INFO);
    if (!link) return -3;
    DeviceLink *chain = link->link;
    next_get_device_proc = chain->get_device_proc;
    link->link = chain->next;
    VkResult (*create)(Handle, const void *, const void *, Handle *) =
        (VkResult(*)(Handle, const void *, const void *, Handle *))chain->get_instance_proc(the_instance, "vkCreateDevice");
    VkResult r = create(physical, info, allocator, out);
    if (r != VK_SUCCESS) return r;
#define RESOLVE(name) next_##name = (typeof(next_##name))next_get_device_proc(*out, #name)
    RESOLVE(vkQueueSubmit);
    RESOLVE(vkQueueSubmit2);
    RESOLVE(vkQueuePresentKHR);
    RESOLVE(vkAcquireNextImageKHR);
    RESOLVE(vkGetSemaphoreCounterValue);
    RESOLVE(vkGetFenceStatus);
    RESOLVE(vkWaitSemaphores);
    RESOLVE(vkWaitForFences);
    RESOLVE(vkDeviceWaitIdle);
    RESOLVE(vkQueueWaitIdle);
    return VK_SUCCESS;
}

static Proc get_device_proc(Handle device, const char *name) {
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (Proc)get_device_proc;
    Proc own = wrapped(name);
    return own ? own : next_get_device_proc(device, name);
}

static Proc get_instance_proc(Handle instance, const char *name) {
    if (!strcmp(name, "vkGetInstanceProcAddr")) return (Proc)get_instance_proc;
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (Proc)get_device_proc;
    if (!strcmp(name, "vkCreateInstance")) return (Proc)create_instance;
    if (!strcmp(name, "vkCreateDevice")) return (Proc)create_device;
    return next_get_instance_proc ? next_get_instance_proc(instance, name) : NULL;
}

EXPORT Proc vkGetInstanceProcAddr(Handle instance, const char *name) {
    return get_instance_proc(instance, name);
}
EXPORT Proc vkGetDeviceProcAddr(Handle device, const char *name) {
    return get_device_proc(device, name);
}
