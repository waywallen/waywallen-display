/*
 * Internal Vulkan backend for libwaywallen_display.
 *
 * Imports DMA-BUF sets as VkImage + VkDeviceMemory and wraps acquire
 * sync_fds as VkSemaphore payloads (temporary SYNC_FD import). The
 * host application must have created its VkDevice with:
 *
 *   VK_KHR_external_memory_fd
 *   VK_EXT_external_memory_dma_buf
 *   VK_EXT_image_drm_format_modifier
 *   VK_KHR_external_semaphore_fd
 *
 * All Vulkan entry points are resolved at runtime: the loader prefers a
 * host-provided vkGetInstanceProcAddr callback when available, and
 * otherwise falls back to dlopen("libvulkan.so.1"). The library never
 * links libvulkan.so directly. Compiled only when WW_HAVE_VULKAN is
 * defined.
 */

#ifndef WAYWALLEN_DISPLAY_BACKEND_VULKAN_H
#define WAYWALLEN_DISPLAY_BACKEND_VULKAN_H

#ifdef WW_HAVE_VULKAN

#include <vulkan/vulkan.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WW_VK_MAX_PLANES 4

typedef struct ww_vk_backend {
    /* Resolved from the host's vkGetInstanceProcAddr. */
    PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr;
    /* Kept for instance-level lookups beyond the initial backend load
     * (e.g. `ww_vk_query_drm_render_node` resolving
     * vkGetPhysicalDeviceProperties2). May be NULL if the loader path
     * never went through the host's get_instance_proc_addr — but in
     * practice we always end up with it because the dlopen fallback
     * resolves it from libvulkan.so.1 too. */
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;

    /* Device-level functions. */
    PFN_vkCreateImage             vkCreateImage;
    PFN_vkDestroyImage            vkDestroyImage;
    PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements;
    PFN_vkAllocateMemory          vkAllocateMemory;
    PFN_vkFreeMemory              vkFreeMemory;
    PFN_vkBindImageMemory         vkBindImageMemory;
    PFN_vkCreateSemaphore         vkCreateSemaphore;
    PFN_vkDestroySemaphore        vkDestroySemaphore;
    /* Drained before tearing down imported VkImages / VkDeviceMemory
     * so the consumer's in-flight command buffers can't outlive their
     * resources (VUID-vkBindImageMemory-image-01445 /
     * BoundResourceFreedMemoryAccess). */
    PFN_vkDeviceWaitIdle          vkDeviceWaitIdle;

    /* Extension functions. */
    PFN_vkGetMemoryFdPropertiesKHR  vkGetMemoryFdPropertiesKHR;
    PFN_vkImportSemaphoreFdKHR     vkImportSemaphoreFdKHR;

    /* Instance-level functions used for diagnostics. Resolved via
     * vkGetInstanceProcAddr(instance, ...). */
    PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties;

    /* VK_EXT_debug_utils — only resolved when the host enabled the
     * instance extension at vkCreateInstance time. NULL otherwise; in
     * that case `debug_messenger` stays VK_NULL_HANDLE and the driver
     * never invokes our callback. */
    PFN_vkCreateDebugUtilsMessengerEXT  vkCreateDebugUtilsMessengerEXT;
    PFN_vkDestroyDebugUtilsMessengerEXT vkDestroyDebugUtilsMessengerEXT;
    VkDebugUtilsMessengerEXT            debug_messenger;

    /* Host-provided handles; NOT owned by the backend. */
    VkInstance       instance;
    VkPhysicalDevice physical_device;
    VkDevice         device;
    uint32_t         queue_family_index;

    bool loaded;
} ww_vk_backend_t;

typedef void *(*ww_vk_get_instance_proc_addr_fn)(void *instance, const char *name);

/* Stringify a VkResult for log lines. Covers the codes we actually
 * see across import / sync / blit paths; unknowns return "VK_<unknown>".
 * Stable strings — safe to embed in log format args directly. */
const char *ww_vk_result_str(VkResult r);

/*
 * `install_debug_utils`: when true (and the host enabled
 * `VK_EXT_debug_utils` at instance creation), register a debug messenger
 * that routes driver/validation messages through `ww_log`. Pass false
 * when another loaded backend on the same VkInstance already installed
 * one (e.g. the blitter sharing the host's instance with the library).
 */
int  ww_vk_backend_load(ww_vk_backend_t *backend,
                        VkInstance instance,
                        VkPhysicalDevice physical_device,
                        VkDevice device,
                        uint32_t queue_family_index,
                        ww_vk_get_instance_proc_addr_fn host_get_proc,
                        bool install_debug_utils);
void ww_vk_backend_unload(ww_vk_backend_t *backend);

/*
 * Look up the DRM render-node major/minor of `physical_device` via
 * `VK_EXT_physical_device_drm` + `VkPhysicalDeviceDrmPropertiesEXT`.
 * The instance must have been created with the property2 extension
 * (Vulkan 1.1+ exposes it as core).
 *
 * Returns 0 on success and writes the values into `*out_major` /
 * `*out_minor`. Returns -ENOSYS if `vkGetPhysicalDeviceProperties2`
 * cannot be resolved or the device doesn't actually carry render-node
 * info (drm property struct's `hasRender` bit is false). Callers should
 * treat any non-zero return as "unknown" and report `(0, 0)`.
 */
int ww_vk_query_drm_render_node(const ww_vk_backend_t *backend,
                                uint32_t *out_major,
                                uint32_t *out_minor);

/* ------------------------------------------------------------------ */
/*  Format/modifier capability probe                                   */
/* ------------------------------------------------------------------ */

/*
 * Streaming emit callback used by `ww_vk_query_format_caps`. Signature
 * mirrors the EGL backend's; consumers can share a single accumulator.
 */
typedef void (*ww_vk_caps_emit_fn)(uint32_t fourcc,
                                   uint64_t modifier,
                                   uint32_t plane_count,
                                   void *user_data);

/*
 * Enumerate (fourcc, modifier) pairs the physical device advertises
 * for `want_features`-capable import via
 * `VK_EXT_image_drm_format_modifier`. Walks a fixed set of common
 * 32-bit RGBA fourccs; for each calls
 * `vkGetPhysicalDeviceFormatProperties2` with
 * `VkDrmFormatModifierPropertiesListEXT` chained in. Reports each
 * (fourcc, modifier) pair whose `drmFormatModifierTilingFeatures`
 * covers `want_features` via `emit`.
 *
 * `want_features` is typically
 * `VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT`
 * for the standard "shadow-import + sample" consumer path; future
 * consumers that import directly into a sampled image may pass a
 * different mask.
 *
 * Returns 0 on success, -ENOSYS if the EXT entry points cannot be
 * resolved on the instance, or -errno from a failed query. On any
 * error `emit` is not invoked.
 */
int ww_vk_query_format_caps(const ww_vk_backend_t *backend,
                            uint32_t want_features,
                            ww_vk_caps_emit_fn emit,
                            void *user_data);

/*
 * Read the `VkPhysicalDeviceIDProperties` deviceUUID + driverUUID
 * for the bound physical device. Each output buffer must point to a
 * 16-byte array. Returns 0 on success, -ENOSYS if
 * `vkGetPhysicalDeviceProperties2` is unavailable.
 */
int ww_vk_query_device_uuid(const ww_vk_backend_t *backend,
                            uint8_t out_device_uuid[16],
                            uint8_t out_driver_uuid[16]);

/*
 * Heuristic: does the bound physical device expose at least one
 * `VkMemoryType` with `VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT`? Used by
 * the display library to OR `WW_MEM_HINT_DEVICE_LOCAL` into its
 * `consumer_caps.mem_hints` so the daemon's same-device picker can
 * land on DEVICE_LOCAL when the producer also asks for it.
 *
 * Whether a specific dmabuf import actually maps to DEVICE_LOCAL
 * depends on the producer's allocation flags (the cross-import
 * memory_type intersection runs at `vkAllocateMemory` time; see
 * `ww_vk_import_dmabuf` in this file). So this is a "DEVICE_LOCAL
 * is *possible*" hint, not a guarantee.
 *
 * Writes 1 / 0 to `*out_has_device_local`. Returns 0 on success,
 * -ENOSYS if `vkGetPhysicalDeviceMemoryProperties` cannot be
 * resolved.
 */
int ww_vk_query_supports_device_local(const ww_vk_backend_t *backend,
                                      int *out_has_device_local);

/* ------------------------------------------------------------------ */

typedef struct ww_vk_dmabuf_import {
    uint32_t fourcc;
    uint32_t width;
    uint32_t height;
    uint64_t modifier;
    uint32_t n_planes;
    int      fds[WW_VK_MAX_PLANES];
    uint32_t strides[WW_VK_MAX_PLANES];
    uint32_t offsets[WW_VK_MAX_PLANES];
} ww_vk_dmabuf_import_t;

typedef struct ww_vk_imported_image {
    VkImage        image;
    VkDeviceMemory memory;
} ww_vk_imported_image_t;

int  ww_vk_import_dmabuf(const ww_vk_backend_t *backend,
                         const ww_vk_dmabuf_import_t *import,
                         ww_vk_imported_image_t *out);
void ww_vk_destroy_imported_image(const ww_vk_backend_t *backend,
                                  ww_vk_imported_image_t *img);

/* ------------------------------------------------------------------ */

/*
 * Import an acquire sync_fd as a temporary semaphore payload.
 * `sem` must be a pre-created VkSemaphore (library manages its
 * lifetime). After this call the semaphore carries the imported
 * fence and should be waited on via `pWaitSemaphores` in the host's
 * next `vkQueueSubmit`. The SYNC_FD import is "temporary": the
 * payload is consumed on the first wait, and the semaphore reverts
 * to its permanent (unsignaled) state.
 *
 * On success: fd ownership transfers to the driver (closed by it).
 * On failure: caller retains the fd and must close it.
 */
int ww_vk_import_sync_fd(const ww_vk_backend_t *backend,
                         VkSemaphore sem,
                         int sync_fd);

#ifdef __cplusplus
}
#endif

#endif /* WW_HAVE_VULKAN */
#endif /* WAYWALLEN_DISPLAY_BACKEND_VULKAN_H */
