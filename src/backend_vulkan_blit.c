/*
 * libwaywallen_display — Vulkan dmabuf -> shadow blitter.
 *
 * Compiled only when WW_HAVE_VULKAN is defined. Reuses
 * ww_vk_backend_t for the device-level fns shared with the dmabuf
 * import path; only the command-recording / fence / submit fns are
 * resolved here.
 */

#ifdef WW_HAVE_VULKAN

#    include "backend_vulkan_blit.h"
#    include "log_internal.h"

#    include <waywallen_display.h>

#    include <errno.h>
#    include <inttypes.h>
#    include <stdint.h>
#    include <string.h>
#    include <sys/ioctl.h>
#    include <unistd.h>

#    ifndef DMA_BUF_BASE
#        define DMA_BUF_BASE 'b'
#    endif
#    ifndef DMA_BUF_IOCTL_IMPORT_SYNC_FILE
/* Field order must match <linux/dma-buf.h> exactly (flags then fd) —
 * the ioctl reads at fixed offsets. */
struct ww_dma_buf_sync_file {
    uint32_t flags;
    int32_t  fd;
};
#        define DMA_BUF_IOCTL_IMPORT_SYNC_FILE _IOW(DMA_BUF_BASE, 3, struct ww_dma_buf_sync_file)
#        define DMA_BUF_SYNC_WRITE             (2u)
#    endif

static uint32_t pick_memory_type(const ww_vk_backend_t* backend, uint32_t type_bits,
                                 VkMemoryPropertyFlags req) {
    if (! backend->vkGetPhysicalDeviceMemoryProperties) return UINT32_MAX;
    VkPhysicalDeviceMemoryProperties props;
    backend->vkGetPhysicalDeviceMemoryProperties(backend->physical_device, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
        if ((type_bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & req) == req) {
            return i;
        }
    }
    return UINT32_MAX;
}

static int resolve_cmd_fns(ww_vk_blitter_t* b) {
    PFN_vkGetDeviceProcAddr gdpa   = b->backend.vkGetDeviceProcAddr;
    VkDevice                device = b->backend.device;

#    define RESOLVE(SLOT, TYPE, NAME)                                                        \
        do {                                                                                 \
            b->SLOT = (TYPE)gdpa(device, NAME);                                              \
            if (! b->SLOT) {                                                                 \
                ww_log(WAYWALLEN_LOG_ERROR, "vk blitter: gdpa(\"%s\") returned NULL", NAME); \
                return -ENOSYS;                                                              \
            }                                                                                \
        } while (0)

    RESOLVE(vkCreateCommandPool, PFN_vkCreateCommandPool, "vkCreateCommandPool");
    RESOLVE(vkDestroyCommandPool, PFN_vkDestroyCommandPool, "vkDestroyCommandPool");
    RESOLVE(vkAllocateCommandBuffers, PFN_vkAllocateCommandBuffers, "vkAllocateCommandBuffers");
    RESOLVE(vkResetCommandPool, PFN_vkResetCommandPool, "vkResetCommandPool");
    RESOLVE(vkBeginCommandBuffer, PFN_vkBeginCommandBuffer, "vkBeginCommandBuffer");
    RESOLVE(vkEndCommandBuffer, PFN_vkEndCommandBuffer, "vkEndCommandBuffer");
    RESOLVE(vkCmdPipelineBarrier, PFN_vkCmdPipelineBarrier, "vkCmdPipelineBarrier");
    RESOLVE(vkCmdCopyImage, PFN_vkCmdCopyImage, "vkCmdCopyImage");
    RESOLVE(vkCreateFence, PFN_vkCreateFence, "vkCreateFence");
    RESOLVE(vkDestroyFence, PFN_vkDestroyFence, "vkDestroyFence");
    RESOLVE(vkResetFences, PFN_vkResetFences, "vkResetFences");
    RESOLVE(vkWaitForFences, PFN_vkWaitForFences, "vkWaitForFences");
    RESOLVE(vkQueueSubmit, PFN_vkQueueSubmit, "vkQueueSubmit");
    RESOLVE(vkQueueWaitIdle, PFN_vkQueueWaitIdle, "vkQueueWaitIdle");

#    undef RESOLVE
    return 0;
}

static int create_cmd_objects(ww_vk_blitter_t* b) {
    VkCommandPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        /* No flags: we recycle the whole pool each frame via vkResetCommandPool. */
        .flags            = 0,
        .queueFamilyIndex = b->backend.queue_family_index,
    };
    VkResult vr = b->vkCreateCommandPool(b->backend.device, &pci, NULL, &b->pool);
    if (vr != VK_SUCCESS) {
        ww_log(WAYWALLEN_LOG_ERROR,
               "vk blitter: vkCreateCommandPool failed: %s",
               ww_vk_result_str(vr));
        return -EIO;
    }

    VkCommandBufferAllocateInfo cbi = {
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool        = b->pool,
        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
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
        ww_log(WAYWALLEN_LOG_ERROR, "vk blitter: vkCreateFence failed: %s", ww_vk_result_str(vr));
        return -EIO;
    }

    /* Exportable signal semaphore (SYNC_FD). Pattern matches GTK's
     * gsk/gpu/gskgpudownloadop.c — signal in submit, vkGetSemaphoreFdKHR
     * gives a real sync_file fd, ioctl-import into dma_resv. */
    VkExportSemaphoreCreateInfo exp_sem = {
        .sType       = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
    };
    VkSemaphoreCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &exp_sem,
    };
    vr = b->backend.vkCreateSemaphore(b->backend.device, &sci, NULL, &b->export_sem);
    if (vr != VK_SUCCESS) {
        ww_log(WAYWALLEN_LOG_ERROR,
               "vk blitter: vkCreateSemaphore(export) failed: %s",
               ww_vk_result_str(vr));
        return -EIO;
    }
    b->vkGetSemaphoreFdKHR = (PFN_vkGetSemaphoreFdKHR)b->backend.vkGetDeviceProcAddr(
        b->backend.device, "vkGetSemaphoreFdKHR");
    if (! b->vkGetSemaphoreFdKHR) {
        ww_log(WAYWALLEN_LOG_ERROR, "vk blitter: vkGetSemaphoreFdKHR not resolvable");
        return -ENOSYS;
    }
    return 0;
}

int ww_vk_blitter_init(ww_vk_blitter_t* b, VkInstance instance, VkPhysicalDevice physical_device,
                       VkDevice device, uint32_t queue_family_index, VkQueue queue,
                       ww_vk_get_instance_proc_addr_fn host_get_proc) {
    if (! b) return -EINVAL;
    if (b->initialized) return 0;
    if (! instance || ! physical_device || ! device || ! queue) {
        ww_log(WAYWALLEN_LOG_ERROR,
               "vk blitter: missing handle "
               "(instance=%p phys=%p device=%p queue=%p)",
               (void*)instance,
               (void*)physical_device,
               (void*)device,
               (void*)queue);
        return -EINVAL;
    }
    memset(b, 0, sizeof(*b));

    int rc = ww_vk_backend_load(&b->backend,
                                instance,
                                physical_device,
                                device,
                                queue_family_index,
                                host_get_proc,
                                /* install_debug_utils */ false);
    if (rc != 0) {
        ww_log(WAYWALLEN_LOG_ERROR, "vk blitter: backend load failed: %d", rc);
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

    b->shadow_export_fd           = -1;
    b->pending_release_syncobj_fd = -1;
    b->initialized                = true;
    ww_log(
        WAYWALLEN_LOG_INFO, "vk blitter ready (qfi=%u queue=%p)", queue_family_index, (void*)queue);
    return 0;
}

static void destroy_shadow(ww_vk_blitter_t* b) {
    if (b->shadow_image != VK_NULL_HANDLE) {
        b->backend.vkDestroyImage(b->backend.device, b->shadow_image, NULL);
        b->shadow_image = VK_NULL_HANDLE;
    }
    if (b->shadow_mem != VK_NULL_HANDLE) {
        b->backend.vkFreeMemory(b->backend.device, b->shadow_mem, NULL);
        b->shadow_mem = VK_NULL_HANDLE;
    }
    if (b->shadow_export_fd >= 0) {
        close(b->shadow_export_fd);
        b->shadow_export_fd = -1;
    }
    b->shadow_export_n_planes = 0;
    b->shadow_w               = 0;
    b->shadow_h               = 0;
    b->shadow_fmt             = VK_FORMAT_UNDEFINED;
    b->shadow_allocation_size = 0;
    b->shadow_has_content     = false;
}

static void destroy_shadow_handles(ww_vk_blitter_t* b, VkImage image, VkDeviceMemory memory) {
    if (image != VK_NULL_HANDLE) {
        b->backend.vkDestroyImage(b->backend.device, image, NULL);
    }
    if (memory != VK_NULL_HANDLE) {
        b->backend.vkFreeMemory(b->backend.device, memory, NULL);
    }
}

static int create_regular_shadow(ww_vk_blitter_t* b, uint32_t w, uint32_t h, VkFormat fmt,
                                 VkImage* out_image, VkDeviceMemory* out_memory,
                                 VkDeviceSize* out_allocation_size) {
    *out_image           = VK_NULL_HANDLE;
    *out_memory          = VK_NULL_HANDLE;
    *out_allocation_size = 0;

    VkImageCreateInfo ici = {
        .sType                 = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType             = VK_IMAGE_TYPE_2D,
        .format                = fmt,
        .extent                = { w, h, 1 },
        .mipLevels             = 1,
        .arrayLayers           = 1,
        .samples               = VK_SAMPLE_COUNT_1_BIT,
        .tiling                = VK_IMAGE_TILING_OPTIMAL,
        .usage                 = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode           = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 1,
        .pQueueFamilyIndices   = &b->backend.queue_family_index,
        .initialLayout         = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkResult vr = b->backend.vkCreateImage(b->backend.device, &ici, NULL, out_image);
    if (vr != VK_SUCCESS) {
        ww_log(WAYWALLEN_LOG_ERROR,
               "vk blitter: vkCreateImage(shadow %ux%u fmt=%d) failed: %s",
               w,
               h,
               (int)fmt,
               ww_vk_result_str(vr));
        return -EIO;
    }

    VkMemoryRequirements req;
    b->backend.vkGetImageMemoryRequirements(b->backend.device, *out_image, &req);

    uint32_t mtype =
        pick_memory_type(&b->backend, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mtype == UINT32_MAX) {
        mtype = pick_memory_type(&b->backend, req.memoryTypeBits, 0);
    }
    if (mtype == UINT32_MAX) {
        ww_log(WAYWALLEN_LOG_ERROR,
               "vk blitter: no memory type for shadow image "
               "(typeBits=0x%08x)",
               req.memoryTypeBits);
        destroy_shadow_handles(b, *out_image, VK_NULL_HANDLE);
        *out_image = VK_NULL_HANDLE;
        return -EIO;
    }

    VkMemoryAllocateInfo mai = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = req.size,
        .memoryTypeIndex = mtype,
    };
    vr = b->backend.vkAllocateMemory(b->backend.device, &mai, NULL, out_memory);
    if (vr != VK_SUCCESS) {
        ww_log(WAYWALLEN_LOG_ERROR,
               "vk blitter: vkAllocateMemory(shadow size=%" PRIu64 ") failed: %s",
               (uint64_t)req.size,
               ww_vk_result_str(vr));
        destroy_shadow_handles(b, *out_image, VK_NULL_HANDLE);
        *out_image = VK_NULL_HANDLE;
        return -EIO;
    }
    vr = b->backend.vkBindImageMemory(b->backend.device, *out_image, *out_memory, 0);
    if (vr != VK_SUCCESS) {
        ww_log(WAYWALLEN_LOG_ERROR,
               "vk blitter: vkBindImageMemory(shadow) failed: %s",
               ww_vk_result_str(vr));
        destroy_shadow_handles(b, *out_image, *out_memory);
        *out_image  = VK_NULL_HANDLE;
        *out_memory = VK_NULL_HANDLE;
        return -EIO;
    }

    ww_log(WAYWALLEN_LOG_INFO,
           "vk blitter: shadow candidate %ux%u fmt=%d ready (mtype=%u size=%" PRIu64 ")",
           w,
           h,
           (int)fmt,
           mtype,
           (uint64_t)req.size);
    *out_allocation_size = req.size;
    return 0;
}

/* Lazily resolve vkGetMemoryFdKHR + vkGetImageSubresourceLayout the
 * first time the relay path needs them. Both are core / KHR entry
 * points on a device with VK_KHR_external_memory_fd. */
static int resolve_export_fns(ww_vk_blitter_t* b) {
    PFN_vkGetDeviceProcAddr gdpa   = b->backend.vkGetDeviceProcAddr;
    VkDevice                device = b->backend.device;
    if (! b->vkGetMemoryFdKHR) {
        b->vkGetMemoryFdKHR = (PFN_vkGetMemoryFdKHR)gdpa(device, "vkGetMemoryFdKHR");
    }
    if (! b->vkGetImageSubresourceLayout) {
        b->vkGetImageSubresourceLayout =
            (PFN_vkGetImageSubresourceLayout)gdpa(device, "vkGetImageSubresourceLayout");
    }
    if (! b->vkGetMemoryFdKHR || ! b->vkGetImageSubresourceLayout) {
        ww_log(WAYWALLEN_LOG_ERROR, "vk blitter: export fns missing (KHR_external_memory_fd?)");
        return -ENOSYS;
    }
    return 0;
}

int ww_vk_blitter_ensure_shadow_exportable(ww_vk_blitter_t* b, uint32_t w, uint32_t h,
                                           VkFormat fmt) {
    if (! b || ! b->initialized) return -EINVAL;
    if (w == 0 || h == 0 || fmt == VK_FORMAT_UNDEFINED) return -EINVAL;
    /* Same shape + already exportable -> nothing to do. */
    if (b->shadow_image != VK_NULL_HANDLE && b->shadow_w == w && b->shadow_h == h &&
        b->shadow_fmt == fmt && b->shadow_export_fd >= 0) {
        return 0;
    }

    int rc = resolve_export_fns(b);
    if (rc != 0) return rc;

    /* Drain any in-flight blit that still references the old shadow. */
    static const uint64_t WW_SHADOW_DRAIN_NS = 2ull * 1000ull * 1000ull * 1000ull;
    if (b->fence_armed) {
        VkResult vrw =
            b->vkWaitForFences(b->backend.device, 1, &b->fence, VK_TRUE, WW_SHADOW_DRAIN_NS);
        if (vrw == VK_TIMEOUT) {
            ww_log(WAYWALLEN_LOG_WARN, "vk blitter: exportable-shadow drain wait timed out");
            return -EIO;
        }
        b->vkResetFences(b->backend.device, 1, &b->fence);
        b->fence_armed = false;
    }
    /* Export consumers own a duplicated dma-buf fd and an independent
     * imported image. Once our copy fence is complete, dropping the
     * local image, memory and fd does not invalidate their payload. */
    destroy_shadow(b);

    VkExternalMemoryImageCreateInfo ext_img = {
        .sType       = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkImageCreateInfo ici = {
        .sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext       = &ext_img,
        .imageType   = VK_IMAGE_TYPE_2D,
        .format      = fmt,
        .extent      = { w, h, 1 },
        .mipLevels   = 1,
        .arrayLayers = 1,
        .samples     = VK_SAMPLE_COUNT_1_BIT,
        /* LINEAR + DRM_FORMAT_MOD_LINEAR is the only safe path without
         * pulling in modifier negotiation; produces a single-plane
         * dmabuf every consumer can import. */
        .tiling                = VK_IMAGE_TILING_LINEAR,
        .usage                 = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode           = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 1,
        .pQueueFamilyIndices   = &b->backend.queue_family_index,
        .initialLayout         = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkResult vr = b->backend.vkCreateImage(b->backend.device, &ici, NULL, &b->shadow_image);
    if (vr != VK_SUCCESS) {
        ww_log(WAYWALLEN_LOG_ERROR,
               "vk blitter: vkCreateImage(exportable %ux%u fmt=%d) failed: %s",
               w,
               h,
               (int)fmt,
               ww_vk_result_str(vr));
        return -EIO;
    }

    VkMemoryRequirements req;
    b->backend.vkGetImageMemoryRequirements(b->backend.device, b->shadow_image, &req);

    uint32_t mtype =
        pick_memory_type(&b->backend, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mtype == UINT32_MAX) {
        /* Some integrated GPUs only expose HOST_VISIBLE for LINEAR. */
        mtype = pick_memory_type(&b->backend, req.memoryTypeBits, 0);
    }
    if (mtype == UINT32_MAX) {
        ww_log(WAYWALLEN_LOG_ERROR,
               "vk blitter: no memory type for exportable shadow "
               "(typeBits=0x%08x)",
               req.memoryTypeBits);
        b->backend.vkDestroyImage(b->backend.device, b->shadow_image, NULL);
        b->shadow_image = VK_NULL_HANDLE;
        return -EIO;
    }

    VkExportMemoryAllocateInfo exp_mem = {
        .sType       = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    /* Dedicated allocation is mandated for many external-memory drivers
     * and harmless otherwise. */
    VkMemoryDedicatedAllocateInfo ded = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .pNext = &exp_mem,
        .image = b->shadow_image,
    };
    VkMemoryAllocateInfo mai = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext           = &ded,
        .allocationSize  = req.size,
        .memoryTypeIndex = mtype,
    };
    vr = b->backend.vkAllocateMemory(b->backend.device, &mai, NULL, &b->shadow_mem);
    if (vr != VK_SUCCESS) {
        ww_log(WAYWALLEN_LOG_ERROR,
               "vk blitter: vkAllocateMemory(exportable size=%" PRIu64 ") failed: %s",
               (uint64_t)req.size,
               ww_vk_result_str(vr));
        b->backend.vkDestroyImage(b->backend.device, b->shadow_image, NULL);
        b->shadow_image = VK_NULL_HANDLE;
        return -EIO;
    }
    vr = b->backend.vkBindImageMemory(b->backend.device, b->shadow_image, b->shadow_mem, 0);
    if (vr != VK_SUCCESS) {
        ww_log(WAYWALLEN_LOG_ERROR,
               "vk blitter: vkBindImageMemory(exportable) failed: %s",
               ww_vk_result_str(vr));
        destroy_shadow(b);
        return -EIO;
    }

    /* Single LINEAR plane: query its row pitch + offset. */
    VkImageSubresource sub = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .mipLevel   = 0,
        .arrayLayer = 0,
    };
    VkSubresourceLayout layout;
    memset(&layout, 0, sizeof(layout));
    b->vkGetImageSubresourceLayout(b->backend.device, b->shadow_image, &sub, &layout);

    VkMemoryGetFdInfoKHR gfd = {
        .sType      = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
        .memory     = b->shadow_mem,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    int dmabuf_fd = -1;
    vr            = b->vkGetMemoryFdKHR(b->backend.device, &gfd, &dmabuf_fd);
    if (vr != VK_SUCCESS || dmabuf_fd < 0) {
        ww_log(
            WAYWALLEN_LOG_ERROR, "vk blitter: vkGetMemoryFdKHR failed: %s", ww_vk_result_str(vr));
        destroy_shadow(b);
        return -EIO;
    }

    b->shadow_w               = w;
    b->shadow_h               = h;
    b->shadow_fmt             = fmt;
    b->shadow_allocation_size = req.size;
    b->shadow_export_fd       = dmabuf_fd;
    b->shadow_export_n_planes = 1;
    /* rowPitch fits in uint32_t for any realistic surface; explicit cast
     * keeps -Wconversion silent. */
    b->shadow_export_strides[0] = (uint32_t)layout.rowPitch;
    b->shadow_export_offsets[0] = (uint64_t)layout.offset;
    b->shadow_export_modifier   = 0ull; /* DRM_FORMAT_MOD_LINEAR */
    b->shadow_has_content       = false;

    ww_log(WAYWALLEN_LOG_INFO,
           "vk blitter: exportable shadow %ux%u fmt=%d ready "
           "(mtype=%u size=%" PRIu64 " fd=%d stride=%u offset=%" PRIu64 ")",
           w,
           h,
           (int)fmt,
           mtype,
           (uint64_t)req.size,
           dmabuf_fd,
           b->shadow_export_strides[0],
           b->shadow_export_offsets[0]);
    return 0;
}

int ww_vk_blitter_get_export(const ww_vk_blitter_t* b, int* out_fd, uint32_t* out_n_planes,
                             uint32_t out_strides[4], uint64_t out_offsets[4],
                             uint64_t* out_modifier) {
    if (! b || ! out_fd || ! out_n_planes || ! out_strides || ! out_offsets || ! out_modifier) {
        return -EINVAL;
    }
    if (b->shadow_export_fd < 0 || b->shadow_export_n_planes == 0) {
        return -EINVAL;
    }
    *out_fd       = b->shadow_export_fd;
    *out_n_planes = b->shadow_export_n_planes;
    for (uint32_t i = 0; i < b->shadow_export_n_planes && i < 4u; i++) {
        out_strides[i] = b->shadow_export_strides[i];
        out_offsets[i] = b->shadow_export_offsets[i];
    }
    *out_modifier = b->shadow_export_modifier;
    return 0;
}

static VkImageSubresourceRange full_color_range(void) {
    VkImageSubresourceRange r = {
        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel   = 0,
        .levelCount     = 1,
        .baseArrayLayer = 0,
        .layerCount     = 1,
    };
    return r;
}

static void resolve_unused_release(int release_syncobj_fd, bool* out_release_armed) {
    if (release_syncobj_fd < 0) return;
    if (waywallen_display_signal_release_syncobj(release_syncobj_fd) == WAYWALLEN_OK &&
        out_release_armed) {
        *out_release_armed = true;
    }
}

static int blit_to_shadow(ww_vk_blitter_t* b, VkImage shadow_image, int shadow_export_fd,
                          VkImage imported, uint32_t w, uint32_t h, VkSemaphore acquire_sem,
                          int release_syncobj_fd, bool* out_release_armed) {
    if (out_release_armed) *out_release_armed = false;
    if (! b || ! b->initialized || shadow_image == VK_NULL_HANDLE) {
        resolve_unused_release(release_syncobj_fd, out_release_armed);
        return -EINVAL;
    }
    if (imported == VK_NULL_HANDLE || w == 0 || h == 0) {
        resolve_unused_release(release_syncobj_fd, out_release_armed);
        return -EINVAL;
    }
    /* 2 s is well above any plausible blit duration (hundreds of µs
     * even on AMD with DCC). Producer death between FrameReady and
     * its acquire dma_fence signal is the only realistic path to a
     * stuck wait — we'd rather log + bail than freeze the QML render
     * thread for 10s+ until the kernel TDR fires. */
    static const uint64_t WW_BLIT_FENCE_WAIT_NS = 2ull * 1000ull * 1000ull * 1000ull;
    if (b->fence_armed) {
        VkResult vrw =
            b->vkWaitForFences(b->backend.device, 1, &b->fence, VK_TRUE, WW_BLIT_FENCE_WAIT_NS);
        if (vrw == VK_TIMEOUT) {
            /* Fence is still in flight; vkResetFences would be UB and
             * submitting a new cmd buffer to the same fence is forbidden.
             * Bail out, keep fence_armed=true. The next call retries. */
            ww_log(WAYWALLEN_LOG_WARN,
                   "vk blitter: pre-submit fence wait timed out (>%llu ms); "
                   "skipping this blit",
                   (unsigned long long)(WW_BLIT_FENCE_WAIT_NS / 1000000ull));
            resolve_unused_release(release_syncobj_fd, out_release_armed);
            return -EIO;
        }
        if (vrw != VK_SUCCESS) {
            ww_log(WAYWALLEN_LOG_WARN,
                   "vk blitter: vkWaitForFences failed: %s",
                   ww_vk_result_str(vrw));
            resolve_unused_release(release_syncobj_fd, out_release_armed);
            return -EIO;
        }
        if (b->pending_release_syncobj_fd >= 0) {
            resolve_unused_release(b->pending_release_syncobj_fd, NULL);
            b->pending_release_syncobj_fd = -1;
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
        resolve_unused_release(release_syncobj_fd, out_release_armed);
        return -EIO;
    }

    /* The producer releases dmabuf images to FOREIGN in GENERAL layout.
     * Acquiring from UNDEFINED discards contents on metadata-backed
     * modifiers such as DCC. */
    VkImageMemoryBarrier in_bar = {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask       = 0,
        .dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout           = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
        .dstQueueFamilyIndex = b->backend.queue_family_index,
        .image               = imported,
        .subresourceRange    = full_color_range(),
    };
    /* Shadow: discard prior layout (we overwrite the whole image).
     * Visibility to the external reader (GSK) is published after submit
     * via DMA_BUF_IOCTL_IMPORT_SYNC_FILE, not via this barrier. */
    VkImageMemoryBarrier shadow_bar0 = {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask       = 0,
        .dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = shadow_image,
        .subresourceRange    = full_color_range(),
    };
    VkImageMemoryBarrier pre_bars[2] = { in_bar, shadow_bar0 };
    b->vkCmdPipelineBarrier(b->cb,
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            0,
                            0,
                            NULL,
                            0,
                            NULL,
                            2,
                            pre_bars);

    VkImageCopy region = {
        .srcSubresource =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        .srcOffset = {0, 0, 0},
        .dstSubresource =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        .dstOffset = {0, 0, 0},
        .extent = {w, h, 1},
    };
    b->vkCmdCopyImage(b->cb,
                      imported,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      shadow_image,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      1,
                      &region);

    VkImageMemoryBarrier out_bar = {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
        .dstAccessMask       = 0,
        .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .newLayout           = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = b->backend.queue_family_index,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
        .image               = imported,
        .subresourceRange    = full_color_range(),
    };
    /* Plain layout transition to SHADER_READ_ONLY_OPTIMAL. The external
     * reader (GSK) gets write-fence visibility via the dma_resv
     * injection below, so QUEUE_FAMILY_IGNORED is correct here. */
    VkImageMemoryBarrier shadow_bar1 = {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask       = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = shadow_image,
        .subresourceRange    = full_color_range(),
    };
    VkImageMemoryBarrier post_bars[2] = { out_bar, shadow_bar1 };
    b->vkCmdPipelineBarrier(b->cb,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT |
                                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                            0,
                            0,
                            NULL,
                            0,
                            NULL,
                            2,
                            post_bars);

    vr = b->vkEndCommandBuffer(b->cb);
    if (vr != VK_SUCCESS) {
        ww_log(
            WAYWALLEN_LOG_ERROR, "vk blitter: vkEndCommandBuffer failed: %s", ww_vk_result_str(vr));
        resolve_unused_release(release_syncobj_fd, out_release_armed);
        return -EIO;
    }

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    const bool           signal_export =
        (shadow_export_fd >= 0 || release_syncobj_fd >= 0) && b->export_sem != VK_NULL_HANDLE;
    VkSubmitInfo si = {
        .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount   = (acquire_sem != VK_NULL_HANDLE) ? 1u : 0u,
        .pWaitSemaphores      = (acquire_sem != VK_NULL_HANDLE) ? &acquire_sem : NULL,
        .pWaitDstStageMask    = (acquire_sem != VK_NULL_HANDLE) ? &wait_stage : NULL,
        .commandBufferCount   = 1,
        .pCommandBuffers      = &b->cb,
        .signalSemaphoreCount = signal_export ? 1u : 0u,
        .pSignalSemaphores    = signal_export ? &b->export_sem : NULL,
    };
    /* Don't try to signal release_syncobj_fd from this submit via
     * vkImportSemaphoreFdKHR(OPAQUE_FD): NVIDIA rejects drm_syncobj
     * fds with "Failed to allocate semaphore device memory". Wait on
     * the fence below and signal the syncobj host-side via
     * waywallen_display_signal_release_syncobj — works on every driver
     * because it's a kernel ioctl. */
    vr = b->vkQueueSubmit(b->queue, 1, &si, b->fence);
    if (vr != VK_SUCCESS) {
        ww_log(WAYWALLEN_LOG_ERROR, "vk blitter: vkQueueSubmit failed: %s", ww_vk_result_str(vr));
        resolve_unused_release(release_syncobj_fd, out_release_armed);
        return -EIO;
    }
    b->fence_armed = true;

    int release_fallback_fd = -1;
    if (release_syncobj_fd >= 0) release_fallback_fd = dup(release_syncobj_fd);
    if (signal_export && b->vkGetSemaphoreFdKHR) {
        int                     sync_fd = -1;
        VkSemaphoreGetFdInfoKHR get     = {
            .sType      = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
            .semaphore  = b->export_sem,
            .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
        };
        VkResult er = b->vkGetSemaphoreFdKHR(b->backend.device, &get, &sync_fd);
        if (er == VK_SUCCESS && sync_fd >= 0) {
            if (shadow_export_fd >= 0) {
                struct ww_dma_buf_sync_file sf = {
                    .flags = DMA_BUF_SYNC_WRITE,
                    .fd    = sync_fd,
                };
                if (ioctl(shadow_export_fd, DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &sf) != 0) {
                    ww_log(WAYWALLEN_LOG_WARN,
                           "vk blitter: dma_buf import_sync_file(fd=%d shadow=%d) failed: %s",
                           sync_fd,
                           shadow_export_fd,
                           strerror(errno));
                }
            }
            if (release_syncobj_fd >= 0 && release_fallback_fd >= 0) {
                int attach_rc =
                    waywallen_display_release_after_sync_file(release_syncobj_fd, sync_fd);
                release_syncobj_fd = -1;
                sync_fd            = -1;
                if (attach_rc == WAYWALLEN_OK) {
                    close(release_fallback_fd);
                    release_fallback_fd = -1;
                    if (out_release_armed) *out_release_armed = true;
                } else {
                    ww_log(WAYWALLEN_LOG_WARN,
                           "vk blitter: attach release sync_file failed: %d",
                           attach_rc);
                }
            }
            if (sync_fd >= 0) close(sync_fd);
        } else {
            ww_log(WAYWALLEN_LOG_WARN,
                   "vk blitter: vkGetSemaphoreFdKHR failed: %s",
                   ww_vk_result_str(er));
        }
    }
    if (release_syncobj_fd >= 0 && release_fallback_fd >= 0) {
        close(release_syncobj_fd);
        release_syncobj_fd = -1;
    } else if (release_syncobj_fd >= 0) {
        release_fallback_fd = release_syncobj_fd;
        release_syncobj_fd  = -1;
    }

    /* Bounded wait: see WW_BLIT_FENCE_WAIT_NS comment above for the
     * 2 s rationale. On VK_TIMEOUT the fence is still in-flight, so
     * we leave fence_armed=true and let the next blit's pre-submit
     * wait try again. The producer slot remains owned until the real
     * submission fence completes. */
    vr = b->vkWaitForFences(b->backend.device, 1, &b->fence, VK_TRUE, WW_BLIT_FENCE_WAIT_NS);
    if (vr == VK_TIMEOUT) {
        ww_log(WAYWALLEN_LOG_WARN,
               "vk blitter: post-submit fence wait timed out (>%llu ms); "
               "shadow may be stale until GPU recovers",
               (unsigned long long)(WW_BLIT_FENCE_WAIT_NS / 1000000ull));
        b->pending_release_syncobj_fd = release_fallback_fd;
        return -ETIMEDOUT;
    }
    if (vr != VK_SUCCESS) {
        ww_log(WAYWALLEN_LOG_ERROR,
               "vk blitter: vkWaitForFences post-submit failed: %s",
               ww_vk_result_str(vr));
        b->pending_release_syncobj_fd = release_fallback_fd;
        return -EIO;
    }
    /* Publish blit's signal as a sync_file → ioctl-import into shadow
     * dmabuf's dma_resv as DMA_BUF_SYNC_WRITE. GSK's later read
     * submission picks it up via kernel implicit DMA-BUF sync, so the
     * long-lived imported VkImage's sampler sees fresh content without
     * us having to rebuild the GdkTexture every frame. Direct copy of
     * the gsk/gpu/gskgpudownloadop.c pattern. */
    b->vkResetFences(b->backend.device, 1, &b->fence);
    b->fence_armed = false;
    if (release_fallback_fd >= 0) {
        int rc = waywallen_display_signal_release_syncobj(release_fallback_fd);
        if (rc != WAYWALLEN_OK) {
            ww_log(
                WAYWALLEN_LOG_WARN, "vk blitter: signal_release_syncobj fallback failed: %d", rc);
        } else if (out_release_armed) {
            *out_release_armed = true;
        }
    }
    return 0;
}

int ww_vk_blitter_blit(ww_vk_blitter_t* b, VkImage imported, uint32_t w, uint32_t h,
                       VkSemaphore acquire_sem, int release_syncobj_fd, bool* out_release_armed) {
    if (out_release_armed) *out_release_armed = false;
    if (! b || ! b->initialized || b->shadow_image == VK_NULL_HANDLE || w != b->shadow_w ||
        h != b->shadow_h) {
        resolve_unused_release(release_syncobj_fd, out_release_armed);
        return -EINVAL;
    }
    int rc = blit_to_shadow(b,
                            b->shadow_image,
                            b->shadow_export_fd,
                            imported,
                            w,
                            h,
                            acquire_sem,
                            release_syncobj_fd,
                            out_release_armed);
    if (rc == 0) b->shadow_has_content = true;
    return rc;
}

int ww_vk_blitter_discard_candidate(ww_vk_blitter_t* b) {
    if (! b || ! b->initialized) return -EINVAL;
    if (b->candidate_image == VK_NULL_HANDLE && b->candidate_mem == VK_NULL_HANDLE) return 0;

    if (b->fence_armed) {
        static const uint64_t WW_CANDIDATE_DRAIN_NS = 2ull * 1000ull * 1000ull * 1000ull;
        VkResult              vr =
            b->vkWaitForFences(b->backend.device, 1, &b->fence, VK_TRUE, WW_CANDIDATE_DRAIN_NS);
        if (vr == VK_TIMEOUT) {
            ww_log(WAYWALLEN_LOG_ERROR,
                   "vk blitter: candidate discard timed out; presentation session must stop");
            return -EBUSY;
        }
        if (vr != VK_SUCCESS) {
            ww_log(WAYWALLEN_LOG_ERROR,
                   "vk blitter: candidate discard wait failed: %s",
                   ww_vk_result_str(vr));
            return -EIO;
        }
        b->vkResetFences(b->backend.device, 1, &b->fence);
        b->fence_armed = false;
    }

    destroy_shadow_handles(b, b->candidate_image, b->candidate_mem);
    b->candidate_image           = VK_NULL_HANDLE;
    b->candidate_mem             = VK_NULL_HANDLE;
    b->candidate_w               = 0;
    b->candidate_h               = 0;
    b->candidate_fmt             = VK_FORMAT_UNDEFINED;
    b->candidate_allocation_size = 0;
    b->candidate_has_content     = false;
    return 0;
}

int ww_vk_blitter_prepare(ww_vk_blitter_t* b, VkImage imported, uint32_t w, uint32_t h,
                          uint32_t fourcc, bool force_replace, VkSemaphore acquire_sem,
                          int release_syncobj_fd, bool* out_candidate_ready,
                          bool* out_release_armed) {
    if (out_candidate_ready) *out_candidate_ready = false;
    if (out_release_armed) *out_release_armed = false;
    const VkFormat fmt = ww_fourcc_to_vk_format(fourcc);
    if (! b || ! b->initialized || w == 0 || h == 0 || fmt == VK_FORMAT_UNDEFINED) {
        resolve_unused_release(release_syncobj_fd, out_release_armed);
        return -EINVAL;
    }
    int rc = ww_vk_blitter_discard_candidate(b);
    if (rc != 0) {
        resolve_unused_release(release_syncobj_fd, out_release_armed);
        return rc;
    }
    if (! force_replace && b->shadow_image != VK_NULL_HANDLE && b->shadow_w == w &&
        b->shadow_h == h && b->shadow_fmt == fmt) {
        return ww_vk_blitter_blit(
            b, imported, w, h, acquire_sem, release_syncobj_fd, out_release_armed);
    }

    rc = create_regular_shadow(
        b, w, h, fmt, &b->candidate_image, &b->candidate_mem, &b->candidate_allocation_size);
    if (rc != 0) {
        resolve_unused_release(release_syncobj_fd, out_release_armed);
        return rc;
    }
    b->candidate_w   = w;
    b->candidate_h   = h;
    b->candidate_fmt = fmt;

    rc = blit_to_shadow(b,
                        b->candidate_image,
                        -1,
                        imported,
                        w,
                        h,
                        acquire_sem,
                        release_syncobj_fd,
                        out_release_armed);
    if (rc != 0) {
        if (! b->fence_armed) (void)ww_vk_blitter_discard_candidate(b);
        return rc;
    }

    b->candidate_has_content = true;
    if (out_candidate_ready) *out_candidate_ready = true;
    return 0;
}

VkResult ww_vk_blitter_commit_candidate(ww_vk_blitter_t* b) {
    if (! b || ! b->initialized || b->candidate_image == VK_NULL_HANDLE ||
        ! b->candidate_has_content) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    if (b->shadow_image != VK_NULL_HANDLE) {
        VkResult vr = b->vkQueueWaitIdle(b->queue);
        if (vr != VK_SUCCESS) {
            ww_log(WAYWALLEN_LOG_ERROR,
                   "vk blitter: vkQueueWaitIdle before shadow retirement failed: %s",
                   ww_vk_result_str(vr));
            return vr;
        }
    }

    destroy_shadow_handles(b, b->shadow_image, b->shadow_mem);
    b->shadow_image           = b->candidate_image;
    b->shadow_mem             = b->candidate_mem;
    b->shadow_w               = b->candidate_w;
    b->shadow_h               = b->candidate_h;
    b->shadow_fmt             = b->candidate_fmt;
    b->shadow_allocation_size = b->candidate_allocation_size;
    b->shadow_has_content     = true;

    b->candidate_image           = VK_NULL_HANDLE;
    b->candidate_mem             = VK_NULL_HANDLE;
    b->candidate_w               = 0;
    b->candidate_h               = 0;
    b->candidate_fmt             = VK_FORMAT_UNDEFINED;
    b->candidate_allocation_size = 0;
    b->candidate_has_content     = false;
    return VK_SUCCESS;
}

int ww_vk_blitter_drain_pending_release(ww_vk_blitter_t* b, bool* out_release_armed) {
    if (out_release_armed) *out_release_armed = false;
    if (! b || b->backend.device == VK_NULL_HANDLE || ! b->backend.vkDeviceWaitIdle) {
        return -EINVAL;
    }
    VkResult idle = b->backend.vkDeviceWaitIdle(b->backend.device);
    if (idle != VK_SUCCESS && idle != VK_ERROR_DEVICE_LOST) {
        ww_log(WAYWALLEN_LOG_WARN,
               "vk blitter: vkDeviceWaitIdle while draining release failed: %s",
               ww_vk_result_str(idle));
        return -EIO;
    }
    if (b->fence_armed) {
        b->vkResetFences(b->backend.device, 1, &b->fence);
        b->fence_armed = false;
    }
    if (b->pending_release_syncobj_fd < 0) return 0;
    int release_fd                = b->pending_release_syncobj_fd;
    b->pending_release_syncobj_fd = -1;
    int rc                        = waywallen_display_signal_release_syncobj(release_fd);
    if (rc != WAYWALLEN_OK) return -EIO;
    if (out_release_armed) *out_release_armed = true;
    return 0;
}

void ww_vk_blitter_shutdown(ww_vk_blitter_t* b) {
    if (! b) return;
    if (! b->initialized && b->pool == VK_NULL_HANDLE && b->fence == VK_NULL_HANDLE &&
        b->shadow_image == VK_NULL_HANDLE) {
        memset(b, 0, sizeof(*b));
        return;
    }
    (void)ww_vk_blitter_drain_pending_release(b, NULL);
    destroy_shadow_handles(b, b->candidate_image, b->candidate_mem);
    b->candidate_image = VK_NULL_HANDLE;
    b->candidate_mem   = VK_NULL_HANDLE;
    destroy_shadow(b);
    if (b->export_sem != VK_NULL_HANDLE && b->backend.vkDestroySemaphore) {
        b->backend.vkDestroySemaphore(b->backend.device, b->export_sem, NULL);
        b->export_sem = VK_NULL_HANDLE;
    }
    if (b->fence != VK_NULL_HANDLE && b->vkDestroyFence) {
        b->vkDestroyFence(b->backend.device, b->fence, NULL);
        b->fence = VK_NULL_HANDLE;
    }
    if (b->pool != VK_NULL_HANDLE && b->vkDestroyCommandPool) {
        b->vkDestroyCommandPool(b->backend.device, b->pool, NULL);
        b->pool = VK_NULL_HANDLE;
    }
    b->cb          = VK_NULL_HANDLE;
    b->fence_armed = false;
    ww_vk_backend_unload(&b->backend);
    memset(b, 0, sizeof(*b));
}

#endif /* WW_HAVE_VULKAN */
