#ifdef WW_HAVE_VULKAN

#include "VkBlitter.hpp"

#include <QLoggingCategory>
#include <unistd.h>

#include <waywallen_display.h>

Q_DECLARE_LOGGING_CATEGORY(lcWD)

namespace {

const char *vkResultStr(VkResult r) {
    switch (r) {
    case VK_SUCCESS:                    return "VK_SUCCESS";
    case VK_NOT_READY:                  return "VK_NOT_READY";
    case VK_TIMEOUT:                    return "VK_TIMEOUT";
    case VK_INCOMPLETE:                 return "VK_INCOMPLETE";
    case VK_ERROR_OUT_OF_HOST_MEMORY:   return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_DEVICE_LOST:          return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_INVALID_EXTERNAL_HANDLE: return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
    default:                            return "VK_<other>";
    }
}

} // namespace

bool VkBlitter::init(VkInstance instance,
                     VkPhysicalDevice phys,
                     VkDevice device,
                     uint32_t qfi,
                     VkQueue queue,
                     PFN_vkGetInstanceProcAddr gipa,
                     PFN_vkGetDeviceProcAddr gdpa) {
    if (m_initialized) return true;
    if (!instance || !phys || !device || !queue || !gipa || !gdpa) {
        qCWarning(lcWD, "VkBlitter::init missing handle "
                  "(instance=%p phys=%p device=%p queue=%p gipa=%p gdpa=%p)",
                  static_cast<void *>(instance), static_cast<void *>(phys),
                  static_cast<void *>(device), static_cast<void *>(queue),
                  reinterpret_cast<void *>(gipa),
                  reinterpret_cast<void *>(gdpa));
        return false;
    }
    m_instance = instance;
    m_phys = phys;
    m_device = device;
    m_qfi = qfi;
    m_queue = queue;
    m_gipa = gipa;
    m_gdpa = gdpa;

    if (!resolveFns())       return false;
    if (!createCmdObjects()) return false;

    m_initialized = true;
    qCInfo(lcWD, "VkBlitter ready (qfi=%u queue=%p)",
           qfi, static_cast<void *>(queue));
    return true;
}

bool VkBlitter::resolveFns() {
#define RESOLVE_INSTANCE(NAME)                                            \
    do {                                                                  \
        m_##NAME = reinterpret_cast<PFN_##NAME>(                          \
            m_gipa(m_instance, #NAME));                                   \
        if (!m_##NAME) {                                                  \
            qCWarning(lcWD, "VkBlitter: gipa(\"%s\") returned NULL", #NAME); \
            return false;                                                 \
        }                                                                 \
    } while (0)

#define RESOLVE_DEVICE(NAME)                                              \
    do {                                                                  \
        m_##NAME = reinterpret_cast<PFN_##NAME>(                          \
            m_gdpa(m_device, #NAME));                                     \
        if (!m_##NAME) {                                                  \
            qCWarning(lcWD, "VkBlitter: gdpa(\"%s\") returned NULL", #NAME); \
            return false;                                                 \
        }                                                                 \
    } while (0)

    RESOLVE_INSTANCE(vkGetPhysicalDeviceMemoryProperties);

    RESOLVE_DEVICE(vkCreateImage);
    RESOLVE_DEVICE(vkDestroyImage);
    RESOLVE_DEVICE(vkGetImageMemoryRequirements);
    RESOLVE_DEVICE(vkAllocateMemory);
    RESOLVE_DEVICE(vkFreeMemory);
    RESOLVE_DEVICE(vkBindImageMemory);
    RESOLVE_DEVICE(vkCreateCommandPool);
    RESOLVE_DEVICE(vkDestroyCommandPool);
    RESOLVE_DEVICE(vkAllocateCommandBuffers);
    RESOLVE_DEVICE(vkResetCommandPool);
    RESOLVE_DEVICE(vkBeginCommandBuffer);
    RESOLVE_DEVICE(vkEndCommandBuffer);
    RESOLVE_DEVICE(vkCmdPipelineBarrier);
    RESOLVE_DEVICE(vkCmdCopyImage);
    RESOLVE_DEVICE(vkCreateFence);
    RESOLVE_DEVICE(vkDestroyFence);
    RESOLVE_DEVICE(vkResetFences);
    RESOLVE_DEVICE(vkWaitForFences);
    RESOLVE_DEVICE(vkQueueSubmit);
    RESOLVE_DEVICE(vkDeviceWaitIdle);

#undef RESOLVE_INSTANCE
#undef RESOLVE_DEVICE
    return true;
}

bool VkBlitter::createCmdObjects() {
    VkCommandPoolCreateInfo pci = {};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = 0; // we use vkResetCommandPool to recycle the whole pool each frame
    pci.queueFamilyIndex = m_qfi;
    VkResult vr = m_vkCreateCommandPool(m_device, &pci, nullptr, &m_pool);
    if (vr != VK_SUCCESS) {
        qCWarning(lcWD, "VkBlitter: vkCreateCommandPool failed: %s",
                  vkResultStr(vr));
        return false;
    }

    VkCommandBufferAllocateInfo cbi = {};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbi.commandPool = m_pool;
    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    vr = m_vkAllocateCommandBuffers(m_device, &cbi, &m_cb);
    if (vr != VK_SUCCESS) {
        qCWarning(lcWD, "VkBlitter: vkAllocateCommandBuffers failed: %s",
                  vkResultStr(vr));
        return false;
    }

    VkFenceCreateInfo fci = {};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.flags = 0;
    vr = m_vkCreateFence(m_device, &fci, nullptr, &m_fence);
    if (vr != VK_SUCCESS) {
        qCWarning(lcWD, "VkBlitter: vkCreateFence failed: %s",
                  vkResultStr(vr));
        return false;
    }
    return true;
}

uint32_t VkBlitter::pickMemoryType(uint32_t typeBits,
                                    VkMemoryPropertyFlags req) const {
    VkPhysicalDeviceMemoryProperties props;
    m_vkGetPhysicalDeviceMemoryProperties(m_phys, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
        if ((typeBits & (1u << i))
            && (props.memoryTypes[i].propertyFlags & req) == req) {
            return i;
        }
    }
    return UINT32_MAX;
}

void VkBlitter::destroyShadow() {
    if (m_shadowImage != VK_NULL_HANDLE) {
        m_vkDestroyImage(m_device, m_shadowImage, nullptr);
        m_shadowImage = VK_NULL_HANDLE;
    }
    if (m_shadowMem != VK_NULL_HANDLE) {
        m_vkFreeMemory(m_device, m_shadowMem, nullptr);
        m_shadowMem = VK_NULL_HANDLE;
    }
    m_shadowW = m_shadowH = 0;
    m_shadowFmt = VK_FORMAT_UNDEFINED;
}

bool VkBlitter::ensureShadow(uint32_t w, uint32_t h, VkFormat fmt) {
    if (!m_initialized) return false;
    if (w == 0 || h == 0 || fmt == VK_FORMAT_UNDEFINED) return false;
    if (m_shadowImage != VK_NULL_HANDLE
        && m_shadowW == w && m_shadowH == h && m_shadowFmt == fmt) {
        return true;
    }
    // Drain any in-flight blit referencing the old shadow before
    // tearing it down.
    if (m_fenceArmed) {
        m_vkWaitForFences(m_device, 1, &m_fence, VK_TRUE, UINT64_MAX);
        m_vkResetFences(m_device, 1, &m_fence);
        m_fenceArmed = false;
    }
    destroyShadow();

    VkImageCreateInfo ici = {};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = fmt;
    ici.extent = { w, h, 1 };
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT
              | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.queueFamilyIndexCount = 1;
    ici.pQueueFamilyIndices = &m_qfi;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkResult vr = m_vkCreateImage(m_device, &ici, nullptr, &m_shadowImage);
    if (vr != VK_SUCCESS) {
        qCWarning(lcWD, "VkBlitter: vkCreateImage(shadow %ux%u fmt=%d) failed: %s",
                  w, h, int(fmt), vkResultStr(vr));
        return false;
    }

    VkMemoryRequirements req;
    m_vkGetImageMemoryRequirements(m_device, m_shadowImage, &req);

    uint32_t mtype = pickMemoryType(req.memoryTypeBits,
                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mtype == UINT32_MAX) {
        // Fall back to any type that satisfies the type bits; some
        // integrated GPUs only expose HOST_VISIBLE memory.
        mtype = pickMemoryType(req.memoryTypeBits, 0);
    }
    if (mtype == UINT32_MAX) {
        qCWarning(lcWD, "VkBlitter: no memory type for shadow image "
                  "(typeBits=0x%08x)", req.memoryTypeBits);
        m_vkDestroyImage(m_device, m_shadowImage, nullptr);
        m_shadowImage = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryAllocateInfo mai = {};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = mtype;
    vr = m_vkAllocateMemory(m_device, &mai, nullptr, &m_shadowMem);
    if (vr != VK_SUCCESS) {
        qCWarning(lcWD, "VkBlitter: vkAllocateMemory(shadow size=%llu) failed: %s",
                  static_cast<unsigned long long>(req.size), vkResultStr(vr));
        m_vkDestroyImage(m_device, m_shadowImage, nullptr);
        m_shadowImage = VK_NULL_HANDLE;
        return false;
    }
    vr = m_vkBindImageMemory(m_device, m_shadowImage, m_shadowMem, 0);
    if (vr != VK_SUCCESS) {
        qCWarning(lcWD, "VkBlitter: vkBindImageMemory(shadow) failed: %s",
                  vkResultStr(vr));
        destroyShadow();
        return false;
    }

    m_shadowW = w;
    m_shadowH = h;
    m_shadowFmt = fmt;
    qCInfo(lcWD, "VkBlitter: shadow %ux%u fmt=%d ready (mtype=%u size=%llu)",
           w, h, int(fmt), mtype, static_cast<unsigned long long>(req.size));
    return true;
}

bool VkBlitter::blit(VkImage imported,
                     uint32_t w, uint32_t h,
                     VkSemaphore acquireSem,
                     int releaseSyncobjFd) {
    if (!m_initialized || m_shadowImage == VK_NULL_HANDLE) {
        if (releaseSyncobjFd >= 0) ::close(releaseSyncobjFd);
        return false;
    }
    if (imported == VK_NULL_HANDLE || w == 0 || h == 0) {
        if (releaseSyncobjFd >= 0) ::close(releaseSyncobjFd);
        return false;
    }
    if (w != m_shadowW || h != m_shadowH) {
        qCWarning(lcWD, "VkBlitter::blit size mismatch (frame=%ux%u shadow=%ux%u)",
                  w, h, m_shadowW, m_shadowH);
        if (releaseSyncobjFd >= 0) ::close(releaseSyncobjFd);
        return false;
    }

    // Drain previous submission so we can recycle CB / fence.
    if (m_fenceArmed) {
        VkResult vrw = m_vkWaitForFences(m_device, 1, &m_fence, VK_TRUE, UINT64_MAX);
        if (vrw != VK_SUCCESS) {
            qCWarning(lcWD, "VkBlitter: vkWaitForFences failed: %s",
                      vkResultStr(vrw));
        }
        m_vkResetFences(m_device, 1, &m_fence);
        m_fenceArmed = false;
    }
    m_vkResetCommandPool(m_device, m_pool, 0);

    VkCommandBufferBeginInfo bi = {};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VkResult vr = m_vkBeginCommandBuffer(m_cb, &bi);
    if (vr != VK_SUCCESS) {
        qCWarning(lcWD, "VkBlitter: vkBeginCommandBuffer failed: %s",
                  vkResultStr(vr));
        return false;
    }

    auto fullSubresource = []() -> VkImageSubresourceRange {
        VkImageSubresourceRange r = {};
        r.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        r.baseMipLevel = 0;
        r.levelCount = 1;
        r.baseArrayLayer = 0;
        r.layerCount = 1;
        return r;
    };

    // Acquire imported image: implicit acquire from EXTERNAL via UNDEFINED layout.
    // Contents of the dmabuf are preserved by the kernel; the layout transition
    // here is purely Vulkan's tracking. After the producer's GPU work has
    // signaled acquireSem, this barrier is safe.
    VkImageMemoryBarrier inBar = {};
    inBar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    inBar.srcAccessMask = 0;
    inBar.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    inBar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    inBar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    inBar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    inBar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    inBar.image = imported;
    inBar.subresourceRange = fullSubresource();

    // Shadow: any prior layout (sampling) -> TRANSFER_DST. UNDEFINED on
    // first frame; subsequent frames also OK to discard, since we
    // overwrite the entire image. srcStage covers prior fragment
    // sampling on the same queue (submission-order memory dependency
    // is satisfied by this barrier on the same queue).
    VkImageMemoryBarrier shadowBar0 = {};
    shadowBar0.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    shadowBar0.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    shadowBar0.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    shadowBar0.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    shadowBar0.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    shadowBar0.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    shadowBar0.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    shadowBar0.image = m_shadowImage;
    shadowBar0.subresourceRange = fullSubresource();

    VkImageMemoryBarrier preBars[2] = { inBar, shadowBar0 };
    m_vkCmdPipelineBarrier(
        m_cb,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
            | VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,
        0, nullptr,
        0, nullptr,
        2, preBars);

    VkImageCopy region = {};
    region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.srcSubresource.mipLevel = 0;
    region.srcSubresource.baseArrayLayer = 0;
    region.srcSubresource.layerCount = 1;
    region.srcOffset = { 0, 0, 0 };
    region.dstSubresource = region.srcSubresource;
    region.dstOffset = { 0, 0, 0 };
    region.extent = { w, h, 1 };
    m_vkCmdCopyImage(
        m_cb,
        imported, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        m_shadowImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1, &region);

    // Shadow: TRANSFER_DST -> SHADER_READ_ONLY for Qt's sampler.
    VkImageMemoryBarrier shadowBar1 = {};
    shadowBar1.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    shadowBar1.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    shadowBar1.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    shadowBar1.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    shadowBar1.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    shadowBar1.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    shadowBar1.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    shadowBar1.image = m_shadowImage;
    shadowBar1.subresourceRange = fullSubresource();
    m_vkCmdPipelineBarrier(
        m_cb,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0,
        0, nullptr,
        0, nullptr,
        1, &shadowBar1);

    vr = m_vkEndCommandBuffer(m_cb);
    if (vr != VK_SUCCESS) {
        qCWarning(lcWD, "VkBlitter: vkEndCommandBuffer failed: %s",
                  vkResultStr(vr));
        return false;
    }

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = {};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = (acquireSem != VK_NULL_HANDLE) ? 1 : 0;
    si.pWaitSemaphores = (acquireSem != VK_NULL_HANDLE) ? &acquireSem : nullptr;
    si.pWaitDstStageMask = (acquireSem != VK_NULL_HANDLE) ? &waitStage : nullptr;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &m_cb;
    // Don't try to signal the drm_syncobj from this submit via
    // vkImportSemaphoreFdKHR(OPAQUE_FD): NVIDIA's driver rejects
    // drm_syncobj fds with "Failed to allocate semaphore device memory"
    // (its OPAQUE_FD payload is not drm_syncobj-compatible). Instead we
    // wait on the fence below and signal the syncobj host-side via
    // waywallen_display_signal_release_syncobj — works on every driver
    // because it's a kernel ioctl. The host-side signal happens AFTER
    // the GPU work completes, so the daemon's reaper observes the
    // correct ordering.

    vr = m_vkQueueSubmit(m_queue, 1, &si, m_fence);
    if (vr != VK_SUCCESS) {
        qCWarning(lcWD, "VkBlitter: vkQueueSubmit failed: %s", vkResultStr(vr));
        if (releaseSyncobjFd >= 0) ::close(releaseSyncobjFd);
        return false;
    }
    m_fenceArmed = true;

    // Wait now so Qt's subsequent sample sees finished writes. CPU
    // stall is acceptable for wallpaper-rate; revisit for compositor
    // overlay use.
    vr = m_vkWaitForFences(m_device, 1, &m_fence, VK_TRUE, UINT64_MAX);
    if (vr != VK_SUCCESS) {
        qCWarning(lcWD, "VkBlitter: vkWaitForFences post-submit failed: %s",
                  vkResultStr(vr));
        if (releaseSyncobjFd >= 0) ::close(releaseSyncobjFd);
        return false;
    }
    m_vkResetFences(m_device, 1, &m_fence);
    m_fenceArmed = false;

    // Signal the drm_syncobj host-side now that the GPU copy is done.
    // The helper signals + closes the fd in all paths (success or failure).
    if (releaseSyncobjFd >= 0) {
        int rc = waywallen_display_signal_release_syncobj(releaseSyncobjFd);
        if (rc != WAYWALLEN_OK) {
            qCWarning(lcWD, "VkBlitter: signal_release_syncobj failed: %d "
                      "(daemon will time out the slot)", rc);
        }
    }
    return true;
}

void VkBlitter::shutdown() {
    if (!m_initialized && m_pool == VK_NULL_HANDLE
        && m_fence == VK_NULL_HANDLE
        && m_shadowImage == VK_NULL_HANDLE) {
        return;
    }
    if (m_device != VK_NULL_HANDLE && m_vkDeviceWaitIdle) {
        m_vkDeviceWaitIdle(m_device);
    }
    destroyShadow();
    if (m_fence != VK_NULL_HANDLE && m_vkDestroyFence) {
        m_vkDestroyFence(m_device, m_fence, nullptr);
        m_fence = VK_NULL_HANDLE;
    }
    if (m_pool != VK_NULL_HANDLE && m_vkDestroyCommandPool) {
        m_vkDestroyCommandPool(m_device, m_pool, nullptr);
        m_pool = VK_NULL_HANDLE;
    }
    m_cb = VK_NULL_HANDLE;
    m_fenceArmed = false;
    m_initialized = false;
}

#endif // WW_HAVE_VULKAN
