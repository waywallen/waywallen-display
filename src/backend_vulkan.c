/*
 * libwaywallen_display — Vulkan backend.
 *
 * Compiled only when WW_HAVE_VULKAN is defined.
 *
 * The library never links libvulkan.so directly; the loader either
 * uses a host-provided vkGetInstanceProcAddr callback or, if that is
 * NULL, dlopen()s libvulkan.so.1 itself and pulls vkGetInstanceProcAddr
 * out of it.
 */

#ifdef WW_HAVE_VULKAN

#define _GNU_SOURCE

#include "backend_vulkan.h"

#include <dlfcn.h>
#include <errno.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Loader                                                             */
/* ------------------------------------------------------------------ */

/*
 * Process-global handle for the dlopen()ed libvulkan.so.1. Left alive
 * across ww_vk_backend_unload() so re-loading is idempotent and we
 * don't race any other component in the same process that holds a
 * resolved fn pointer.
 */
static void *s_libvulkan = NULL;

int ww_vk_backend_load(ww_vk_backend_t *backend,
                       VkInstance instance,
                       VkPhysicalDevice physical_device,
                       VkDevice device,
                       uint32_t queue_family_index,
                       ww_vk_get_instance_proc_addr_fn host_get_proc) {
    if (!backend) return -EINVAL;
    memset(backend, 0, sizeof(*backend));

    backend->instance = instance;
    backend->physical_device = physical_device;
    backend->device = device;
    backend->queue_family_index = queue_family_index;

    /* POSIX.1 §dlsym: object/function pointer conversion through a
     * union to keep -Wpedantic happy. */
    union { void *obj; void (*func)(void); } cvt;

    /* Step 1: obtain a vkGetDeviceProcAddr.
     *
     * Priority:
     *   host_get_proc(instance, "vkGetDeviceProcAddr")
     *     — preferred when the host has already resolved the loader;
     *   dlopen("libvulkan.so.1") + dlsym("vkGetInstanceProcAddr")
     *     followed by gipa(instance, "vkGetDeviceProcAddr")
     *     — fallback, so consumers don't need to link libvulkan. */
    PFN_vkGetDeviceProcAddr gdpa = NULL;

    if (host_get_proc) {
        cvt.obj = host_get_proc(instance, "vkGetDeviceProcAddr");
        if (cvt.obj) gdpa = (PFN_vkGetDeviceProcAddr)cvt.func;
    }

    if (!gdpa) {
        if (!s_libvulkan) {
            s_libvulkan = dlopen("libvulkan.so.1", RTLD_LAZY | RTLD_LOCAL);
        }
        if (!s_libvulkan) return -ENOENT;
        cvt.obj = dlsym(s_libvulkan, "vkGetInstanceProcAddr");
        if (!cvt.obj) return -ENOSYS;
        PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)cvt.func;
        /* gipa returns PFN_vkVoidFunction (a function pointer); cast
         * directly to PFN_vkGetDeviceProcAddr — function-to-function
         * pointer conversion is permitted, function-to-object is not. */
        PFN_vkVoidFunction vf = gipa(instance, "vkGetDeviceProcAddr");
        if (!vf) return -ENOSYS;
        gdpa = (PFN_vkGetDeviceProcAddr)vf;
    }
    backend->vkGetDeviceProcAddr = gdpa;

/* Convenience: resolve a device-level function or fail. */
#define RESOLVE(SLOT, TYPE, NAME)                                      \
    do {                                                               \
        backend->SLOT = (TYPE)gdpa(device, NAME);                      \
        if (!backend->SLOT) return -ENOSYS;                            \
    } while (0)

    /* Core device functions. */
    RESOLVE(vkCreateImage,             PFN_vkCreateImage,             "vkCreateImage");
    RESOLVE(vkDestroyImage,            PFN_vkDestroyImage,            "vkDestroyImage");
    RESOLVE(vkGetImageMemoryRequirements, PFN_vkGetImageMemoryRequirements, "vkGetImageMemoryRequirements");
    RESOLVE(vkAllocateMemory,          PFN_vkAllocateMemory,          "vkAllocateMemory");
    RESOLVE(vkFreeMemory,              PFN_vkFreeMemory,              "vkFreeMemory");
    RESOLVE(vkBindImageMemory,         PFN_vkBindImageMemory,         "vkBindImageMemory");
    RESOLVE(vkCreateSemaphore,         PFN_vkCreateSemaphore,         "vkCreateSemaphore");
    RESOLVE(vkDestroySemaphore,        PFN_vkDestroySemaphore,        "vkDestroySemaphore");

    /* Extension: VK_KHR_external_memory_fd. */
    RESOLVE(vkGetMemoryFdPropertiesKHR, PFN_vkGetMemoryFdPropertiesKHR,
            "vkGetMemoryFdPropertiesKHR");
    /* Extension: VK_KHR_external_semaphore_fd. */
    RESOLVE(vkImportSemaphoreFdKHR,    PFN_vkImportSemaphoreFdKHR,
            "vkImportSemaphoreFdKHR");

#undef RESOLVE

    backend->loaded = true;
    return 0;
}

void ww_vk_backend_unload(ww_vk_backend_t *backend) {
    if (backend) {
        memset(backend, 0, sizeof(*backend));
    }
}

/* ------------------------------------------------------------------ */
/*  DMA-BUF import                                                     */
/* ------------------------------------------------------------------ */

/*
 * Map DRM fourcc codes to VkFormat. This is a minimal table covering
 * the formats the Rust `waywallen_renderer` and the C++ ExSwapchain
 * currently produce. Extend as needed.
 */
static VkFormat drm_fourcc_to_vk(uint32_t fourcc) {
    switch (fourcc) {
        /* DRM_FORMAT_ARGB8888 / XRGB8888 — B8G8R8A8 in Vulkan order. */
        case 0x34325241: /* AR24 */
        case 0x34325258: /* XR24 */
            return VK_FORMAT_B8G8R8A8_UNORM;
        /* DRM_FORMAT_ABGR8888 / XBGR8888 — R8G8B8A8. */
        case 0x34324241: /* AB24 */
        case 0x34324258: /* XB24 */
            return VK_FORMAT_R8G8B8A8_UNORM;
        default:
            return VK_FORMAT_UNDEFINED;
    }
}

int ww_vk_import_dmabuf(const ww_vk_backend_t *backend,
                        const ww_vk_dmabuf_import_t *im,
                        ww_vk_imported_image_t *out) {
    if (!backend || !im || !out) return -EINVAL;
    if (!backend->loaded) return -ENOSYS;
    if (im->n_planes == 0 || im->n_planes > WW_VK_MAX_PLANES) return -EINVAL;

    memset(out, 0, sizeof(*out));

    VkFormat format = drm_fourcc_to_vk(im->fourcc);
    if (format == VK_FORMAT_UNDEFINED) return -EINVAL;

    /* Build per-plane layout descriptors. */
    VkSubresourceLayout plane_layouts[WW_VK_MAX_PLANES];
    memset(plane_layouts, 0, sizeof(plane_layouts));
    for (uint32_t p = 0; p < im->n_planes; p++) {
        plane_layouts[p].offset   = im->offsets[p];
        plane_layouts[p].rowPitch = im->strides[p];
        plane_layouts[p].size     = 0; /* driver computes */
    }

    VkImageDrmFormatModifierExplicitCreateInfoEXT mod_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
        .pNext = NULL,
        .drmFormatModifier = im->modifier,
        .drmFormatModifierPlaneCount = im->n_planes,
        .pPlaneLayouts = plane_layouts,
    };

    VkExternalMemoryImageCreateInfo ext_mem_info = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext = &mod_info,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };

    VkImageCreateInfo image_ci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &ext_mem_info,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = { im->width, im->height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 1,
        .pQueueFamilyIndices = &backend->queue_family_index,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    VkResult vr = backend->vkCreateImage(backend->device, &image_ci,
                                         NULL, &out->image);
    if (vr != VK_SUCCESS) return -EIO;

    VkMemoryRequirements mem_req;
    backend->vkGetImageMemoryRequirements(backend->device, out->image,
                                         &mem_req);

    /* Query which memory types accept this DMA-BUF fd. */
    VkMemoryFdPropertiesKHR fd_props = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
    };
    vr = backend->vkGetMemoryFdPropertiesKHR(
        backend->device,
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        im->fds[0],
        &fd_props);
    if (vr != VK_SUCCESS) {
        backend->vkDestroyImage(backend->device, out->image, NULL);
        out->image = VK_NULL_HANDLE;
        return -EIO;
    }

    /* Pick the first memory type that satisfies both the image and the
     * fd's compatibility masks. */
    uint32_t mem_type = UINT32_MAX;
    uint32_t mask = mem_req.memoryTypeBits & fd_props.memoryTypeBits;
    for (uint32_t i = 0; i < 32 && mask; i++) {
        if (mask & (1u << i)) {
            mem_type = i;
            break;
        }
    }
    if (mem_type == UINT32_MAX) {
        backend->vkDestroyImage(backend->device, out->image, NULL);
        out->image = VK_NULL_HANDLE;
        return -EIO;
    }

    VkImportMemoryFdInfoKHR import_fd = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        .fd = im->fds[0],
    };

    VkMemoryAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &import_fd,
        .allocationSize = mem_req.size,
        .memoryTypeIndex = mem_type,
    };

    vr = backend->vkAllocateMemory(backend->device, &alloc_info,
                                   NULL, &out->memory);
    if (vr != VK_SUCCESS) {
        backend->vkDestroyImage(backend->device, out->image, NULL);
        out->image = VK_NULL_HANDLE;
        return -EIO;
    }
    /* On success vkAllocateMemory took ownership of the fd — the
     * driver will close it. Do not close it ourselves. */

    vr = backend->vkBindImageMemory(backend->device, out->image,
                                    out->memory, 0);
    if (vr != VK_SUCCESS) {
        backend->vkFreeMemory(backend->device, out->memory, NULL);
        backend->vkDestroyImage(backend->device, out->image, NULL);
        memset(out, 0, sizeof(*out));
        return -EIO;
    }

    return 0;
}

void ww_vk_destroy_imported_image(const ww_vk_backend_t *backend,
                                  ww_vk_imported_image_t *img) {
    if (!backend || !backend->loaded || !img) return;
    if (img->image != VK_NULL_HANDLE) {
        backend->vkDestroyImage(backend->device, img->image, NULL);
    }
    if (img->memory != VK_NULL_HANDLE) {
        backend->vkFreeMemory(backend->device, img->memory, NULL);
    }
    memset(img, 0, sizeof(*img));
}

/* ------------------------------------------------------------------ */
/*  Sync_fd import                                                     */
/* ------------------------------------------------------------------ */

int ww_vk_import_sync_fd(const ww_vk_backend_t *backend,
                         VkSemaphore sem,
                         int sync_fd) {
    if (!backend || !backend->loaded) return -ENOSYS;
    if (sem == VK_NULL_HANDLE || sync_fd < 0) return -EINVAL;

    VkImportSemaphoreFdInfoKHR import_info = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
        .semaphore = sem,
        .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
        .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
        .fd = sync_fd,
    };

    VkResult vr = backend->vkImportSemaphoreFdKHR(backend->device,
                                                   &import_info);
    if (vr != VK_SUCCESS) return -EIO;

    /* On success the driver owns the fd. */
    return 0;
}

#else /* !WW_HAVE_VULKAN */

typedef int ww_vk_backend_disabled_t;

#endif /* WW_HAVE_VULKAN */
