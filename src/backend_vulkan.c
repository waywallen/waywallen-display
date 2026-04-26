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
#include "log_internal.h"

#include <dlfcn.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/*  Diagnostics helpers                                                */
/* ------------------------------------------------------------------ */

/* Stringify a VkResult for log output. Covers the codes we actually see
 * out of the import / sync paths; anything else falls through to the
 * raw integer. Single-source-of-truth for our log lines so failures are
 * grep-able by name (e.g. VK_ERROR_INVALID_EXTERNAL_HANDLE). */
static const char *vk_result_str(VkResult r) {
    switch (r) {
    case VK_SUCCESS:                              return "VK_SUCCESS";
    case VK_NOT_READY:                            return "VK_NOT_READY";
    case VK_TIMEOUT:                              return "VK_TIMEOUT";
    case VK_EVENT_SET:                            return "VK_EVENT_SET";
    case VK_EVENT_RESET:                          return "VK_EVENT_RESET";
    case VK_INCOMPLETE:                           return "VK_INCOMPLETE";
    case VK_ERROR_OUT_OF_HOST_MEMORY:             return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:           return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED:          return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST:                    return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED:              return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT:              return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT:          return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT:            return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER:            return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS:               return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED:           return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_FRAGMENTED_POOL:                return "VK_ERROR_FRAGMENTED_POOL";
    case VK_ERROR_UNKNOWN:                        return "VK_ERROR_UNKNOWN";
    case VK_ERROR_OUT_OF_POOL_MEMORY:             return "VK_ERROR_OUT_OF_POOL_MEMORY";
    case VK_ERROR_INVALID_EXTERNAL_HANDLE:        return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
    case VK_ERROR_FRAGMENTATION:                  return "VK_ERROR_FRAGMENTATION";
    case VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS: return "VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS";
    case VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT:
        return "VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT";
    default:                                      return "VK_<unknown>";
    }
}

/* Format VkMemoryPropertyFlags as a short human-readable bitlist into
 * `buf`. `buf_len` must be at least 96 bytes for the full list. */
static const char *mem_props_str(VkMemoryPropertyFlags f, char *buf, size_t buf_len) {
    if (buf_len == 0) return "";
    buf[0] = '\0';
    size_t n = 0;
#define APPEND(name) do {                                              \
        if (f & VK_MEMORY_PROPERTY_##name##_BIT) {                     \
            int w = snprintf(buf + n, buf_len - n,                     \
                             "%s" #name, n ? "|" : "");                \
            if (w < 0 || (size_t)w >= buf_len - n) return buf;         \
            n += (size_t)w;                                            \
        }                                                              \
    } while (0)
    APPEND(DEVICE_LOCAL);
    APPEND(HOST_VISIBLE);
    APPEND(HOST_COHERENT);
    APPEND(HOST_CACHED);
    APPEND(LAZILY_ALLOCATED);
    APPEND(PROTECTED);
#undef APPEND
    if (n == 0) snprintf(buf, buf_len, "0");
    return buf;
}

/* Map a Vulkan debug-utils severity onto our log level. */
static waywallen_log_level_t debug_severity_to_log_level(
    VkDebugUtilsMessageSeverityFlagBitsEXT s) {
    if (s & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) return WAYWALLEN_LOG_ERROR;
    if (s & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) return WAYWALLEN_LOG_WARN;
    if (s & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) return WAYWALLEN_LOG_INFO;
    return WAYWALLEN_LOG_DEBUG;
}

/* Driver/validation message sink. Called from arbitrary driver threads;
 * ww_log itself dispatches through a user-installed callback that is
 * expected to be reentrancy-safe. */
static VKAPI_ATTR VkBool32 VKAPI_CALL vk_debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT     severity,
    VkDebugUtilsMessageTypeFlagsEXT            types,
    const VkDebugUtilsMessengerCallbackDataEXT *data,
    void                                       *user_data) {
    (void)user_data;
    if (!data) return VK_FALSE;
    const char *kind =
        (types & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) ? "validation"
        : (types & VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT) ? "perf"
        : (types & VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT) ? "general"
        : "?";
    ww_log(debug_severity_to_log_level(severity),
           "vk[%s] %s: %s", kind,
           data->pMessageIdName ? data->pMessageIdName : "(no-id)",
           data->pMessage ? data->pMessage : "(no-message)");
    return VK_FALSE; /* never abort the calling Vulkan command */
}

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
    if (!backend) {
        ww_log(WAYWALLEN_LOG_ERROR, "vk backend load: NULL backend");
        return -EINVAL;
    }
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
    PFN_vkGetInstanceProcAddr gipa = NULL;

    if (host_get_proc) {
        cvt.obj = host_get_proc(instance, "vkGetInstanceProcAddr");
        if (cvt.obj) gipa = (PFN_vkGetInstanceProcAddr)cvt.func;
        cvt.obj = host_get_proc(instance, "vkGetDeviceProcAddr");
        if (cvt.obj) gdpa = (PFN_vkGetDeviceProcAddr)cvt.func;
    }

    if (!gipa || !gdpa) {
        if (!s_libvulkan) {
            s_libvulkan = dlopen("libvulkan.so.1", RTLD_LAZY | RTLD_LOCAL);
        }
        if (!s_libvulkan) {
            ww_log(WAYWALLEN_LOG_ERROR,
                   "vk backend load: dlopen(libvulkan.so.1) failed: %s",
                   dlerror());
            return -ENOENT;
        }
        if (!gipa) {
            cvt.obj = dlsym(s_libvulkan, "vkGetInstanceProcAddr");
            if (!cvt.obj) {
                ww_log(WAYWALLEN_LOG_ERROR,
                       "vk backend load: dlsym(vkGetInstanceProcAddr) failed");
                return -ENOSYS;
            }
            gipa = (PFN_vkGetInstanceProcAddr)cvt.func;
        }
        if (!gdpa) {
            /* gipa returns PFN_vkVoidFunction (a function pointer);
             * cast directly to PFN_vkGetDeviceProcAddr — function-to-
             * function pointer conversion is permitted, function-to-
             * object is not. */
            PFN_vkVoidFunction vf = gipa(instance, "vkGetDeviceProcAddr");
            if (!vf) {
                ww_log(WAYWALLEN_LOG_ERROR,
                       "vk backend load: gipa(\"vkGetDeviceProcAddr\") returned NULL");
                return -ENOSYS;
            }
            gdpa = (PFN_vkGetDeviceProcAddr)vf;
        }
    }
    backend->vkGetInstanceProcAddr = gipa;
    backend->vkGetDeviceProcAddr = gdpa;

/* Convenience: resolve a device-level function or fail. */
#define RESOLVE(SLOT, TYPE, NAME)                                      \
    do {                                                               \
        backend->SLOT = (TYPE)gdpa(device, NAME);                      \
        if (!backend->SLOT) {                                          \
            ww_log(WAYWALLEN_LOG_ERROR,                                \
                   "vk backend load: gdpa(\"%s\") returned NULL — "    \
                   "host did not enable the required extension/version", \
                   NAME);                                              \
            return -ENOSYS;                                            \
        }                                                              \
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

    /* Instance-level diagnostics fns. Resolve via gipa; failure here is
     * non-fatal — we only use them for prettier log lines. */
    backend->vkGetPhysicalDeviceMemoryProperties =
        (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(
            instance, "vkGetPhysicalDeviceMemoryProperties");

    /* VK_EXT_debug_utils — only present if the host enabled the
     * instance extension at vkCreateInstance time. Both fns must
     * resolve for the messenger to be installable. */
    backend->vkCreateDebugUtilsMessengerEXT =
        (PFN_vkCreateDebugUtilsMessengerEXT)gipa(
            instance, "vkCreateDebugUtilsMessengerEXT");
    backend->vkDestroyDebugUtilsMessengerEXT =
        (PFN_vkDestroyDebugUtilsMessengerEXT)gipa(
            instance, "vkDestroyDebugUtilsMessengerEXT");

    if (backend->vkCreateDebugUtilsMessengerEXT
        && backend->vkDestroyDebugUtilsMessengerEXT) {
        VkDebugUtilsMessengerCreateInfoEXT msg_ci = {
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity =
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType =
                VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
            .pfnUserCallback = vk_debug_callback,
            .pUserData       = NULL,
        };
        VkResult mvr = backend->vkCreateDebugUtilsMessengerEXT(
            instance, &msg_ci, NULL, &backend->debug_messenger);
        if (mvr == VK_SUCCESS) {
            ww_log(WAYWALLEN_LOG_INFO,
                   "vk debug_utils messenger installed (severity=verbose+up)");
        } else {
            ww_log(WAYWALLEN_LOG_WARN,
                   "vk debug_utils messenger create failed: %s; continuing without driver messages",
                   vk_result_str(mvr));
            backend->debug_messenger = VK_NULL_HANDLE;
        }
    } else {
        ww_log(WAYWALLEN_LOG_DEBUG,
               "vk debug_utils not available (host did not enable VK_EXT_debug_utils); driver messages disabled");
    }

    backend->loaded = true;
    return 0;
}

void ww_vk_backend_unload(ww_vk_backend_t *backend) {
    if (!backend) return;
    if (backend->debug_messenger != VK_NULL_HANDLE
        && backend->vkDestroyDebugUtilsMessengerEXT
        && backend->instance != VK_NULL_HANDLE) {
        backend->vkDestroyDebugUtilsMessengerEXT(
            backend->instance, backend->debug_messenger, NULL);
        ww_log(WAYWALLEN_LOG_DEBUG, "vk debug_utils messenger destroyed");
    }
    memset(backend, 0, sizeof(*backend));
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
    if (!backend || !im || !out) {
        ww_log(WAYWALLEN_LOG_ERROR,
               "vk import: NULL arg (backend=%p im=%p out=%p)",
               (void *)backend, (const void *)im, (void *)out);
        return -EINVAL;
    }
    if (!backend->loaded) {
        ww_log(WAYWALLEN_LOG_ERROR, "vk import: backend not loaded");
        return -ENOSYS;
    }
    if (im->n_planes == 0 || im->n_planes > WW_VK_MAX_PLANES) {
        ww_log(WAYWALLEN_LOG_ERROR,
               "vk import: invalid n_planes=%u (max=%d)",
               im->n_planes, WW_VK_MAX_PLANES);
        return -EINVAL;
    }

    memset(out, 0, sizeof(*out));

    ww_log(WAYWALLEN_LOG_DEBUG,
           "vk import: fourcc=0x%08x %ux%u modifier=0x%" PRIx64 " n_planes=%u fd0=%d "
           "stride0=%u offset0=%u",
           im->fourcc, im->width, im->height, im->modifier, im->n_planes,
           im->fds[0], im->strides[0], im->offsets[0]);

    VkFormat format = drm_fourcc_to_vk(im->fourcc);
    if (format == VK_FORMAT_UNDEFINED) {
        ww_log(WAYWALLEN_LOG_ERROR,
               "vk import: unsupported DRM fourcc 0x%08x", im->fourcc);
        return -EINVAL;
    }

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
    if (vr != VK_SUCCESS) {
        ww_log(WAYWALLEN_LOG_ERROR,
               "vk import: vkCreateImage failed: %s "
               "(fourcc=0x%08x %ux%u modifier=0x%" PRIx64 " n_planes=%u) — "
               "driver rejected the explicit modifier+plane layout; check "
               "that the producer's modifier is in the importer's supported list",
               vk_result_str(vr), im->fourcc, im->width, im->height,
               im->modifier, im->n_planes);
        return -EIO;
    }

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
        ww_log(WAYWALLEN_LOG_ERROR,
               "vk import: vkGetMemoryFdPropertiesKHR(fd=%d) failed: %s — "
               "fd is not a valid dmabuf or driver rejected the handle type",
               im->fds[0], vk_result_str(vr));
        backend->vkDestroyImage(backend->device, out->image, NULL);
        out->image = VK_NULL_HANDLE;
        return -EIO;
    }

    /* Candidate memtypes: intersection of (image-compatible) and
     * (fd-compatible). The fd-compatibility mask from the driver is
     * advisory — some advertised types still fail at allocate-time
     * (e.g. an "external-import-only" slot with property flags = 0),
     * so we must iterate and try each instead of trusting the first
     * set bit. Retries dup the fd; on success the driver owns the dup
     * and we close the caller's original to honor the existing
     * contract that fds[0] is consumed on success. */
    const uint32_t mask = mem_req.memoryTypeBits & fd_props.memoryTypeBits;
    if (mask == 0) {
        ww_log(WAYWALLEN_LOG_ERROR,
               "vk import: no memory type accepts both image and fd "
               "(image.memoryTypeBits=0x%08x fd.memoryTypeBits=0x%08x intersection=0) — "
               "typical cross-GPU PRIME failure when producer allocated "
               "DEVICE_LOCAL VRAM instead of HOST_VISIBLE GTT",
               mem_req.memoryTypeBits, fd_props.memoryTypeBits);
        backend->vkDestroyImage(backend->device, out->image, NULL);
        out->image = VK_NULL_HANDLE;
        return -EIO;
    }

    /* Snapshot the device's memtype table once for pretty-printing. */
    VkPhysicalDeviceMemoryProperties pdmp;
    bool has_pdmp = false;
    if (backend->vkGetPhysicalDeviceMemoryProperties) {
        backend->vkGetPhysicalDeviceMemoryProperties(
            backend->physical_device, &pdmp);
        has_pdmp = true;
    }

    ww_log(WAYWALLEN_LOG_DEBUG,
           "vk import: candidate memtypes mask=0x%08x "
           "(image.bits=0x%08x fd.bits=0x%08x) size=%" PRIu64,
           mask, mem_req.memoryTypeBits, fd_props.memoryTypeBits,
           (uint64_t)mem_req.size);

    uint32_t   chosen_index    = UINT32_MAX;
    char       chosen_props[96] = "?";
    VkResult   last_vr         = VK_ERROR_UNKNOWN;
    /* Track whether vkAllocateMemory consumed the original fd on the
     * winning attempt (when we re-used the caller's fd as the LAST try
     * and it succeeded). In every other success path we close the
     * caller's fd ourselves below. */
    bool       original_fd_consumed = false;

    /* Build an ordered list so we can use the caller's original fd on
     * the final attempt — saves one dup per import in the common case. */
    uint32_t candidates[32];
    uint32_t n_cand = 0;
    for (uint32_t i = 0; i < 32; i++) {
        if (mask & (1u << i)) candidates[n_cand++] = i;
    }

    for (uint32_t k = 0; k < n_cand; k++) {
        const uint32_t i = candidates[k];
        const bool     last_attempt = (k + 1 == n_cand);

        char props_buf[96] = "?";
        if (has_pdmp && i < pdmp.memoryTypeCount) {
            mem_props_str(pdmp.memoryTypes[i].propertyFlags,
                          props_buf, sizeof(props_buf));
        }

        int try_fd;
        if (last_attempt) {
            try_fd = im->fds[0];
        } else {
            try_fd = dup(im->fds[0]);
            if (try_fd < 0) {
                ww_log(WAYWALLEN_LOG_WARN,
                       "vk import: dup(fd=%d) failed: %s; skipping memtype %u",
                       im->fds[0], strerror(errno), i);
                continue;
            }
        }

        VkImportMemoryFdInfoKHR import_fd = {
            .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
            .fd = try_fd,
        };
        VkMemoryAllocateInfo alloc_info = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext = &import_fd,
            .allocationSize = mem_req.size,
            .memoryTypeIndex = i,
        };

        VkResult vr2 = backend->vkAllocateMemory(
            backend->device, &alloc_info, NULL, &out->memory);
        if (vr2 == VK_SUCCESS) {
            chosen_index = i;
            memcpy(chosen_props, props_buf, sizeof(chosen_props));
            original_fd_consumed = last_attempt;
            ww_log(WAYWALLEN_LOG_DEBUG,
                   "vk import: vkAllocateMemory ok memTypeIndex=%u props=[%s]%s",
                   i, props_buf,
                   last_attempt ? " (consumed original fd)" : " (consumed dup fd)");
            break;
        }

        last_vr = vr2;
        if (!last_attempt) close(try_fd);
        ww_log(WAYWALLEN_LOG_DEBUG,
               "vk import: vkAllocateMemory(memTypeIndex=%u props=[%s]) failed: %s; %s",
               i, props_buf, vk_result_str(vr2),
               last_attempt ? "out of candidates" : "trying next memtype");
    }

    if (chosen_index == UINT32_MAX) {
        ww_log(WAYWALLEN_LOG_ERROR,
               "vk import: vkAllocateMemory exhausted all %u candidate memtype(s) "
               "(mask=0x%08x size=%" PRIu64 "), last error: %s",
               n_cand, mask, (uint64_t)mem_req.size, vk_result_str(last_vr));
        backend->vkDestroyImage(backend->device, out->image, NULL);
        out->image = VK_NULL_HANDLE;
        return -EIO;
    }

    /* Honor the contract: caller's fds[0] is consumed on success. If
     * the winning attempt used a dup, close the caller's original now;
     * otherwise the driver already owns it. */
    if (!original_fd_consumed) close(im->fds[0]);

    vr = backend->vkBindImageMemory(backend->device, out->image,
                                    out->memory, 0);
    if (vr != VK_SUCCESS) {
        ww_log(WAYWALLEN_LOG_ERROR,
               "vk import: vkBindImageMemory failed: %s", vk_result_str(vr));
        backend->vkFreeMemory(backend->device, out->memory, NULL);
        backend->vkDestroyImage(backend->device, out->image, NULL);
        memset(out, 0, sizeof(*out));
        return -EIO;
    }

    ww_log(WAYWALLEN_LOG_DEBUG,
           "vk import: success (image=%p memory=%p memTypeIndex=%u props=[%s])",
           (void *)(uintptr_t)out->image, (void *)(uintptr_t)out->memory,
           chosen_index, chosen_props);
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
    if (!backend || !backend->loaded) {
        ww_log(WAYWALLEN_LOG_ERROR,
               "vk import_sync_fd: backend not loaded");
        return -ENOSYS;
    }
    if (sem == VK_NULL_HANDLE || sync_fd < 0) {
        ww_log(WAYWALLEN_LOG_ERROR,
               "vk import_sync_fd: invalid arg (sem=%p sync_fd=%d)",
               (void *)(uintptr_t)sem, sync_fd);
        return -EINVAL;
    }

    VkImportSemaphoreFdInfoKHR import_info = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
        .semaphore = sem,
        .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
        .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
        .fd = sync_fd,
    };

    VkResult vr = backend->vkImportSemaphoreFdKHR(backend->device,
                                                   &import_info);
    if (vr != VK_SUCCESS) {
        ww_log(WAYWALLEN_LOG_ERROR,
               "vk import_sync_fd: vkImportSemaphoreFdKHR(sync_fd=%d) failed: %s",
               sync_fd, vk_result_str(vr));
        return -EIO;
    }

    /* On success the driver owns the fd. */
    return 0;
}

/* ------------------------------------------------------------------ */
/*  DRM render-node introspection                                      */
/* ------------------------------------------------------------------ */

/* VK_EXT_physical_device_drm is part of every modern Vulkan SDK
 * (vulkan_core.h defines `VkPhysicalDeviceDrmPropertiesEXT` and the
 * matching sType). We rely on those definitions; if the host's SDK is
 * old enough to lack them, the build will fail loudly here rather
 * than silently produce a wire mismatch. */

int ww_vk_query_drm_render_node(const ww_vk_backend_t *backend,
                                uint32_t *out_major,
                                uint32_t *out_minor) {
    if (!backend || !backend->loaded || !out_major || !out_minor) {
        return -EINVAL;
    }
    if (!backend->vkGetInstanceProcAddr) return -ENOSYS;

    PFN_vkVoidFunction p2_vf = backend->vkGetInstanceProcAddr(
        backend->instance, "vkGetPhysicalDeviceProperties2");
    if (!p2_vf) {
        p2_vf = backend->vkGetInstanceProcAddr(
            backend->instance, "vkGetPhysicalDeviceProperties2KHR");
    }
    if (!p2_vf) return -ENOSYS;
    PFN_vkGetPhysicalDeviceProperties2 gpdp2 =
        (PFN_vkGetPhysicalDeviceProperties2)p2_vf;

    VkPhysicalDeviceDrmPropertiesEXT drm = {0};
    drm.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT;
    drm.pNext = NULL;

    VkPhysicalDeviceProperties2 props = {0};
    props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props.pNext = &drm;

    gpdp2(backend->physical_device, &props);

    if (!drm.hasRender) return -ENOSYS;
    if (drm.renderMajor < 0 || drm.renderMinor < 0) return -ENOSYS;
    if ((uint64_t)drm.renderMajor > UINT32_MAX
        || (uint64_t)drm.renderMinor > UINT32_MAX) {
        return -EINVAL;
    }
    *out_major = (uint32_t)drm.renderMajor;
    *out_minor = (uint32_t)drm.renderMinor;
    return 0;
}

#else /* !WW_HAVE_VULKAN */

typedef int ww_vk_backend_disabled_t;

#endif /* WW_HAVE_VULKAN */
