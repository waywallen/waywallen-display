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
 * All Vulkan entry points are resolved at runtime via the host-provided
 * vkGetInstanceProcAddr; the library never links libvulkan.so directly.
 * Compiled only when WW_HAVE_VULKAN is defined.
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

    /* Device-level functions. */
    PFN_vkCreateImage             vkCreateImage;
    PFN_vkDestroyImage            vkDestroyImage;
    PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements;
    PFN_vkAllocateMemory          vkAllocateMemory;
    PFN_vkFreeMemory              vkFreeMemory;
    PFN_vkBindImageMemory         vkBindImageMemory;
    PFN_vkCreateSemaphore         vkCreateSemaphore;
    PFN_vkDestroySemaphore        vkDestroySemaphore;

    /* Extension functions. */
    PFN_vkGetMemoryFdPropertiesKHR  vkGetMemoryFdPropertiesKHR;
    PFN_vkImportSemaphoreFdKHR     vkImportSemaphoreFdKHR;

    /* Host-provided handles; NOT owned by the backend. */
    VkInstance       instance;
    VkPhysicalDevice physical_device;
    VkDevice         device;
    uint32_t         queue_family_index;

    bool loaded;
} ww_vk_backend_t;

typedef void *(*ww_vk_get_instance_proc_addr_fn)(void *instance, const char *name);

int  ww_vk_backend_load(ww_vk_backend_t *backend,
                        VkInstance instance,
                        VkPhysicalDevice physical_device,
                        VkDevice device,
                        uint32_t queue_family_index,
                        ww_vk_get_instance_proc_addr_fn host_get_proc);
void ww_vk_backend_unload(ww_vk_backend_t *backend);

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
