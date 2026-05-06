/*
 * libwaywallen_display — Vulkan dmabuf -> shadow blitter.
 *
 * Compiled only when WW_HAVE_VULKAN is defined. Reuses
 * ww_vk_backend_t for the device-level fns shared with the dmabuf
 * import path; only the command-recording / fence / submit fns are
 * resolved here.
 */

#ifdef WW_HAVE_VULKAN

#include "backend_vulkan_blit.h"
#include "log_internal.h"

#include <waywallen_display.h>

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

static uint32_t pick_memory_type(const ww_vk_backend_t *backend,
                                 uint32_t type_bits,
                                 VkMemoryPropertyFlags req) {
    if (!backend->vkGetPhysicalDeviceMemoryProperties) return UINT32_MAX;
    VkPhysicalDeviceMemoryProperties props;
    backend->vkGetPhysicalDeviceMemoryProperties(backend->physical_device,
                                                 &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
        if ((type_bits & (1u << i))
            && (props.memoryTypes[i].propertyFlags & req) == req) {
            return i;
        }
    }
    return UINT32_MAX;
}

static int resolve_cmd_fns(ww_vk_blitter_t *b) {
    PFN_vkGetDeviceProcAddr gdpa = b->backend.vkGetDeviceProcAddr;
    VkDevice device = b->backend.device;

#define RESOLVE(SLOT, TYPE, NAME)                                          \
    do {                                                                   \
        b->SLOT = (TYPE)gdpa(device, NAME);                                \
        if (!b->SLOT) {                                                    \
            ww_log(WAYWALLEN_LOG_ERROR,                                    \
                   "vk blitter: gdpa(\"%s\") returned NULL", NAME);        \
            return -ENOSYS;                                                \
        }                                                                  \
    } while (0)

    RESOLVE(vkCreateCommandPool,      PFN_vkCreateCommandPool,      "vkCreateCommandPool");
    RESOLVE(vkDestroyCommandPool,     PFN_vkDestroyCommandPool,     "vkDestroyCommandPool");
    RESOLVE(vkAllocateCommandBuffers, PFN_vkAllocateCommandBuffers, "vkAllocateCommandBuffers");
    RESOLVE(vkResetCommandPool,       PFN_vkResetCommandPool,       "vkResetCommandPool");
    RESOLVE(vkBeginCommandBuffer,     PFN_vkBeginCommandBuffer,     "vkBeginCommandBuffer");
    RESOLVE(vkEndCommandBuffer,       PFN_vkEndCommandBuffer,       "vkEndCommandBuffer");
    RESOLVE(vkCmdPipelineBarrier,     PFN_vkCmdPipelineBarrier,     "vkCmdPipelineBarrier");
    RESOLVE(vkCmdCopyImage,           PFN_vkCmdCopyImage,           "vkCmdCopyImage");
    RESOLVE(vkCreateFence,            PFN_vkCreateFence,            "vkCreateFence");
    RESOLVE(vkDestroyFence,           PFN_vkDestroyFence,           "vkDestroyFence");
    RESOLVE(vkResetFences,            PFN_vkResetFences,            "vkResetFences");
    RESOLVE(vkWaitForFences,          PFN_vkWaitForFences,          "vkWaitForFences");
    RESOLVE(vkQueueSubmit,            PFN_vkQueueSubmit,            "vkQueueSubmit");

#undef RESOLVE
    return 0;
}

static int create_cmd_objects(ww_vk_blitter_t *b) {
    VkCommandPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        /* No flags: we recycle the whole pool each frame via vkResetCommandPool. */
        .flags = 0,
        .queueFamilyIndex = b->backend.queue_family_index,
    };
    VkResult vr = b->vkCreateCommandPool(b->backend.device, &pci, NULL,
                                          &b->pool);
    if (vr != VK_SUCCESS) {
        ww_log(WAYWALLEN_LOG_ERROR,
               "vk blitter: vkCreateCommandPool failed: %s",
               ww_vk_result_str(vr));
        return -EIO;
    }

    VkCommandBufferAllocateInfo cbi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = b->pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    vr = b->vkAllocateCommandBuffers(b->backend.device, &cbi, &b->cb);
    if (vr != VK_SUCCESS) {
        ww_log(WAYWALLEN_LOG_ERROR,
               "vk blitter: vkAllocateCommandBuffers failed: %s",
               ww_vk_result_str(vr));
        return -EIO;
    }

    VkFenceCreateInfo fci = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = 0,
    };
    vr = b->vkCreateFence(b->backend.device, &fci, NULL, &b->fence);
    if (vr != VK_SUCCESS) {
        ww_log(WAYWALLEN_LOG_ERROR,
               "vk blitter: vkCreateFence failed: %s",
               ww_vk_result_str(vr));
        return -EIO;
    }
    return 0;
}

int ww_vk_blitter_init(ww_vk_blitter_t *b,
                       VkInstance instance,
                       VkPhysicalDevice physical_device,
                       VkDevice device,
                       uint32_t queue_family_index,
                       VkQueue queue,
                       ww_vk_get_instance_proc_addr_fn host_get_proc) {
    if (!b) return -EINVAL;
    if (b->initialized) return 0;
    if (!instance || !physical_device || !device || !queue) {
        ww_log(WAYWALLEN_LOG_ERROR,
               "vk blitter: missing handle "
               "(instance=%p phys=%p device=%p queue=%p)",
               (void *)instance, (void *)physical_device,
               (void *)device, (void *)queue);
        return -EINVAL;
    }
    memset(b, 0, sizeof(*b));

    int rc = ww_vk_backend_load(&b->backend, instance, physical_device,
                                device, queue_family_index, host_get_proc,
                                /* install_debug_utils */ false);
    if (rc != 0) {
        ww_log(WAYWALLEN_LOG_ERROR,
               "vk blitter: backend load failed: %d", rc);
        return rc;
    }
    b->queue = queue;

    rc = resolve_cmd_fns(b);
    if (rc != 0) {
        ww_vk_backend_unload(&b->backend);
        return rc;
    }
    rc = create_cmd_objects(b);
    if (rc != 0) {
        ww_vk_blitter_shutdown(b);
        return rc;
    }

    b->initialized = true;
    ww_log(WAYWALLEN_LOG_INFO,
           "vk blitter ready (qfi=%u queue=%p)",
           queue_family_index, (void *)queue);
    return 0;
}

static void destroy_shadow(ww_vk_blitter_t *b) {
    if (b->shadow_image != VK_NULL_HANDLE) {
        b->backend.vkDestroyImage(b->backend.device, b->shadow_image, NULL);
        b->shadow_image = VK_NULL_HANDLE;
    }
    if (b->shadow_mem != VK_NULL_HANDLE) {
        b->backend.vkFreeMemory(b->backend.device, b->shadow_mem, NULL);
        b->shadow_mem = VK_NULL_HANDLE;
    }
    b->shadow_w = 0;
    b->shadow_h = 0;
    b->shadow_fmt = VK_FORMAT_UNDEFINED;
}

int ww_vk_blitter_ensure_shadow(ww_vk_blitter_t *b,
                                uint32_t w, uint32_t h, VkFormat fmt) {
    if (!b || !b->initialized) return -EINVAL;
    if (w == 0 || h == 0 || fmt == VK_FORMAT_UNDEFINED) return -EINVAL;
    if (b->shadow_image != VK_NULL_HANDLE
        && b->shadow_w == w && b->shadow_h == h && b->shadow_fmt == fmt) {
        return 0;
    }

    /* Drain any in-flight blit referencing the old shadow before
     * tearing it down. */
    if (b->fence_armed) {
        b->vkWaitForFences(b->backend.device, 1, &b->fence, VK_TRUE,
                           UINT64_MAX);
        b->vkResetFences(b->backend.device, 1, &b->fence);
        b->fence_armed = false;
    }
    destroy_shadow(b);

    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = fmt,
        .extent = { w, h, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT
               | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 1,
        .pQueueFamilyIndices = &b->backend.queue_family_index,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkResult vr = b->backend.vkCreateImage(b->backend.device, &ici, NULL,
                                           &b->shadow_image);
    if (vr != VK_SUCCESS) {
        ww_log(WAYWALLEN_LOG_ERROR,
               "vk blitter: vkCreateImage(shadow %ux%u fmt=%d) failed: %s",
               w, h, (int)fmt, ww_vk_result_str(vr));
        return -EIO;
    }

    VkMemoryRequirements req;
    b->backend.vkGetImageMemoryRequirements(b->backend.device,
                                            b->shadow_image, &req);

    /* Some integrated GPUs only expose HOST_VISIBLE for the bits we
     * need; fall back to "any" matching type when DEVICE_LOCAL fails. */
    uint32_t mtype = pick_memory_type(&b->backend, req.memoryTypeBits,
                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mtype == UINT32_MAX) {
        mtype = pick_memory_type(&b->backend, req.memoryTypeBits, 0);
    }
    if (mtype == UINT32_MAX) {
        ww_log(WAYWALLEN_LOG_ERROR,
               "vk blitter: no memory type for shadow image "
               "(typeBits=0x%08x)", req.memoryTypeBits);
        b->backend.vkDestroyImage(b->backend.device, b->shadow_image, NULL);
        b->shadow_image = VK_NULL_HANDLE;
        return -EIO;
    }

    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = mtype,
    };
    vr = b->backend.vkAllocateMemory(b->backend.device, &mai, NULL,
                                     &b->shadow_mem);
    if (vr != VK_SUCCESS) {
        ww_log(WAYWALLEN_LOG_ERROR,
               "vk blitter: vkAllocateMemory(shadow size=%" PRIu64 ") failed: %s",
               (uint64_t)req.size, ww_vk_result_str(vr));
        b->backend.vkDestroyImage(b->backend.device, b->shadow_image, NULL);
        b->shadow_image = VK_NULL_HANDLE;
        return -EIO;
    }
    vr = b->backend.vkBindImageMemory(b->backend.device, b->shadow_image,
                                      b->shadow_mem, 0);
    if (vr != VK_SUCCESS) {
        ww_log(WAYWALLEN_LOG_ERROR,
               "vk blitter: vkBindImageMemory(shadow) failed: %s",
               ww_vk_result_str(vr));
        destroy_shadow(b);
        return -EIO;
    }

    b->shadow_w = w;
    b->shadow_h = h;
    b->shadow_fmt = fmt;
    ww_log(WAYWALLEN_LOG_INFO,
           "vk blitter: shadow %ux%u fmt=%d ready (mtype=%u size=%" PRIu64 ")",
           w, h, (int)fmt, mtype, (uint64_t)req.size);
    return 0;
}

static VkImageSubresourceRange full_color_range(void) {
    VkImageSubresourceRange r = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
    return r;
}

int ww_vk_blitter_blit(ww_vk_blitter_t *b,
                       VkImage imported,
                       uint32_t w, uint32_t h,
                       VkSemaphore acquire_sem,
                       int release_syncobj_fd) {
    if (!b || !b->initialized || b->shadow_image == VK_NULL_HANDLE) {
        if (release_syncobj_fd >= 0) close(release_syncobj_fd);
        return -EINVAL;
    }
    if (imported == VK_NULL_HANDLE || w == 0 || h == 0) {
        if (release_syncobj_fd >= 0) close(release_syncobj_fd);
        return -EINVAL;
    }
    if (w != b->shadow_w || h != b->shadow_h) {
        ww_log(WAYWALLEN_LOG_WARN,
               "vk blitter: size mismatch (frame=%ux%u shadow=%ux%u)",
               w, h, b->shadow_w, b->shadow_h);
        if (release_syncobj_fd >= 0) close(release_syncobj_fd);
        return -EINVAL;
    }

    if (b->fence_armed) {
        VkResult vrw = b->vkWaitForFences(b->backend.device, 1, &b->fence,
                                           VK_TRUE, UINT64_MAX);
        if (vrw != VK_SUCCESS) {
            ww_log(WAYWALLEN_LOG_WARN,
                   "vk blitter: vkWaitForFences failed: %s",
                   ww_vk_result_str(vrw));
        }
        b->vkResetFences(b->backend.device, 1, &b->fence);
        b->fence_armed = false;
    }
    b->vkResetCommandPool(b->backend.device, b->pool, 0);

    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VkResult vr = b->vkBeginCommandBuffer(b->cb, &bi);
    if (vr != VK_SUCCESS) {
        ww_log(WAYWALLEN_LOG_ERROR,
               "vk blitter: vkBeginCommandBuffer failed: %s",
               ww_vk_result_str(vr));
        if (release_syncobj_fd >= 0) close(release_syncobj_fd);
        return -EIO;
    }

    /* Acquire imported image: implicit acquire from EXTERNAL via
     * UNDEFINED layout. After acquire_sem signals, the producer's
     * GPU work is visible, so this barrier is safe. */
    VkImageMemoryBarrier in_bar = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = imported,
        .subresourceRange = full_color_range(),
    };
    /* Shadow: any prior layout (sampling) -> TRANSFER_DST. UNDEFINED on
     * first frame; subsequent frames also OK to discard, since we
     * overwrite the entire image. */
    VkImageMemoryBarrier shadow_bar0 = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = b->shadow_image,
        .subresourceRange = full_color_range(),
    };
    VkImageMemoryBarrier pre_bars[2] = { in_bar, shadow_bar0 };
    b->vkCmdPipelineBarrier(b->cb,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
            | VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, NULL, 0, NULL, 2, pre_bars);

    VkImageCopy region = {
        .srcSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
        .srcOffset = { 0, 0, 0 },
        .dstSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
        .dstOffset = { 0, 0, 0 },
        .extent = { w, h, 1 },
    };
    b->vkCmdCopyImage(b->cb,
        imported, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        b->shadow_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1, &region);

    VkImageMemoryBarrier shadow_bar1 = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = b->shadow_image,
        .subresourceRange = full_color_range(),
    };
    b->vkCmdPipelineBarrier(b->cb,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, NULL, 0, NULL, 1, &shadow_bar1);

    vr = b->vkEndCommandBuffer(b->cb);
    if (vr != VK_SUCCESS) {
        ww_log(WAYWALLEN_LOG_ERROR,
               "vk blitter: vkEndCommandBuffer failed: %s",
               ww_vk_result_str(vr));
        if (release_syncobj_fd >= 0) close(release_syncobj_fd);
        return -EIO;
    }

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = (acquire_sem != VK_NULL_HANDLE) ? 1u : 0u,
        .pWaitSemaphores = (acquire_sem != VK_NULL_HANDLE) ? &acquire_sem : NULL,
        .pWaitDstStageMask = (acquire_sem != VK_NULL_HANDLE) ? &wait_stage : NULL,
        .commandBufferCount = 1,
        .pCommandBuffers = &b->cb,
    };
    /* Don't try to signal release_syncobj_fd from this submit via
     * vkImportSemaphoreFdKHR(OPAQUE_FD): NVIDIA rejects drm_syncobj
     * fds with "Failed to allocate semaphore device memory". Wait on
     * the fence below and signal the syncobj host-side via
     * waywallen_display_signal_release_syncobj — works on every driver
     * because it's a kernel ioctl. */
    vr = b->vkQueueSubmit(b->queue, 1, &si, b->fence);
    if (vr != VK_SUCCESS) {
        ww_log(WAYWALLEN_LOG_ERROR,
               "vk blitter: vkQueueSubmit failed: %s",
               ww_vk_result_str(vr));
        if (release_syncobj_fd >= 0) close(release_syncobj_fd);
        return -EIO;
    }
    b->fence_armed = true;

    /* Wait now so subsequent sample sees finished writes. CPU stall
     * is acceptable for wallpaper-rate; revisit for compositor overlay. */
    vr = b->vkWaitForFences(b->backend.device, 1, &b->fence, VK_TRUE,
                            UINT64_MAX);
    if (vr != VK_SUCCESS) {
        ww_log(WAYWALLEN_LOG_ERROR,
               "vk blitter: vkWaitForFences post-submit failed: %s",
               ww_vk_result_str(vr));
        if (release_syncobj_fd >= 0) close(release_syncobj_fd);
        return -EIO;
    }
    b->vkResetFences(b->backend.device, 1, &b->fence);
    b->fence_armed = false;

    if (release_syncobj_fd >= 0) {
        int rc = waywallen_display_signal_release_syncobj(release_syncobj_fd);
        if (rc != WAYWALLEN_OK) {
            ww_log(WAYWALLEN_LOG_WARN,
                   "vk blitter: signal_release_syncobj failed: %d "
                   "(daemon will time out the slot)", rc);
        }
    }
    return 0;
}

void ww_vk_blitter_shutdown(ww_vk_blitter_t *b) {
    if (!b) return;
    if (!b->initialized && b->pool == VK_NULL_HANDLE
        && b->fence == VK_NULL_HANDLE
        && b->shadow_image == VK_NULL_HANDLE) {
        memset(b, 0, sizeof(*b));
        return;
    }
    if (b->backend.device != VK_NULL_HANDLE && b->backend.vkDeviceWaitIdle) {
        b->backend.vkDeviceWaitIdle(b->backend.device);
    }
    destroy_shadow(b);
    if (b->fence != VK_NULL_HANDLE && b->vkDestroyFence) {
        b->vkDestroyFence(b->backend.device, b->fence, NULL);
        b->fence = VK_NULL_HANDLE;
    }
    if (b->pool != VK_NULL_HANDLE && b->vkDestroyCommandPool) {
        b->vkDestroyCommandPool(b->backend.device, b->pool, NULL);
        b->pool = VK_NULL_HANDLE;
    }
    b->cb = VK_NULL_HANDLE;
    b->fence_armed = false;
    ww_vk_backend_unload(&b->backend);
    memset(b, 0, sizeof(*b));
}

#endif /* WW_HAVE_VULKAN */
