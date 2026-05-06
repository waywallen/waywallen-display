/*
 * Vulkan dmabuf -> shadow image blitter for libwaywallen_display hosts.
 *
 * Imported dmabuf VkImages are created with TRANSFER_SRC usage only
 * (per-modifier format features often exclude SAMPLED on vendor
 * tilings). The host needs a sampler-friendly OPTIMAL VkImage; this
 * blitter owns that "shadow" and copies each frame into it on the
 * host's queue, then host-signals the daemon's release_syncobj after
 * the GPU copy completes.
 *
 * Reuses ww_vk_backend_t for the device-level fns it shares with the
 * dmabuf import path; resolves command-recording / fence / submit
 * fns separately.
 */

#ifndef WAYWALLEN_DISPLAY_BACKEND_VULKAN_BLIT_H
#define WAYWALLEN_DISPLAY_BACKEND_VULKAN_BLIT_H

#ifdef WW_HAVE_VULKAN

#include "backend_vulkan.h"

#include <vulkan/vulkan.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ww_vk_blitter {
    /* Embedded backend, loaded with install_debug_utils=false to avoid
     * doubling up driver log forwarding when the same VkInstance is
     * already bound via waywallen_display_bind_vulkan. */
    ww_vk_backend_t backend;
    VkQueue         queue;

    PFN_vkCreateCommandPool      vkCreateCommandPool;
    PFN_vkDestroyCommandPool     vkDestroyCommandPool;
    PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers;
    PFN_vkResetCommandPool       vkResetCommandPool;
    PFN_vkBeginCommandBuffer     vkBeginCommandBuffer;
    PFN_vkEndCommandBuffer       vkEndCommandBuffer;
    PFN_vkCmdPipelineBarrier     vkCmdPipelineBarrier;
    PFN_vkCmdCopyImage           vkCmdCopyImage;
    PFN_vkCreateFence            vkCreateFence;
    PFN_vkDestroyFence           vkDestroyFence;
    PFN_vkResetFences            vkResetFences;
    PFN_vkWaitForFences          vkWaitForFences;
    PFN_vkQueueSubmit            vkQueueSubmit;

    VkCommandPool   pool;
    VkCommandBuffer cb;
    VkFence         fence;
    bool            fence_armed;

    VkImage         shadow_image;
    VkDeviceMemory  shadow_mem;
    uint32_t        shadow_w;
    uint32_t        shadow_h;
    VkFormat        shadow_fmt;

    bool initialized;
} ww_vk_blitter_t;

/*
 * Initialize. Returns 0 on success, negative errno on failure (struct
 * left zeroed). `host_get_proc` is the same callback shape backend_vulkan
 * uses; pass NULL to fall back to dlopen("libvulkan.so.1").
 */
int  ww_vk_blitter_init(ww_vk_blitter_t *b,
                        VkInstance instance,
                        VkPhysicalDevice physical_device,
                        VkDevice device,
                        uint32_t queue_family_index,
                        VkQueue queue,
                        ww_vk_get_instance_proc_addr_fn host_get_proc);

/* Idempotent, safe to call on a zero-initialized struct. */
void ww_vk_blitter_shutdown(ww_vk_blitter_t *b);

/*
 * (Re-)create the shadow image when (w, h, fmt) differ from the
 * current one. No-op when they match. Returns 0 on success, negative
 * errno on failure.
 */
int  ww_vk_blitter_ensure_shadow(ww_vk_blitter_t *b,
                                 uint32_t w, uint32_t h, VkFormat fmt);

/*
 * Copy `imported` (UNDEFINED layout, TRANSFER_SRC contents valid) into
 * the shadow. Waits on `acquire_sem` (may be VK_NULL_HANDLE), then
 * blocks the calling thread until the copy completes (vkWaitForFences).
 *
 * `release_syncobj_fd` ownership transfers in: signaled host-side via
 * waywallen_display_signal_release_syncobj after the wait, or closed
 * on early failure. Pass -1 if the caller has no syncobj to signal.
 *
 * Returns 0 on success, negative errno on failure.
 */
int  ww_vk_blitter_blit(ww_vk_blitter_t *b,
                        VkImage imported,
                        uint32_t w, uint32_t h,
                        VkSemaphore acquire_sem,
                        int release_syncobj_fd);

static inline VkImage ww_vk_blitter_shadow(const ww_vk_blitter_t *b) {
    return b ? b->shadow_image : VK_NULL_HANDLE;
}

static inline VkImageLayout ww_vk_blitter_shadow_layout(const ww_vk_blitter_t *b) {
    (void)b;
    return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

static inline bool ww_vk_blitter_initialized(const ww_vk_blitter_t *b) {
    return b && b->initialized;
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* WW_HAVE_VULKAN */
#endif /* WAYWALLEN_DISPLAY_BACKEND_VULKAN_BLIT_H */
