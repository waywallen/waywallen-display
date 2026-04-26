#pragma once

#ifdef WW_HAVE_VULKAN

#include <vulkan/vulkan.h>
#include <stdint.h>

class VkBlitter {
public:
    VkBlitter() = default;
    ~VkBlitter() { shutdown(); }

    VkBlitter(const VkBlitter &) = delete;
    VkBlitter &operator=(const VkBlitter &) = delete;

    bool init(VkInstance instance,
              VkPhysicalDevice phys,
              VkDevice device,
              uint32_t queueFamilyIndex,
              VkQueue queue,
              PFN_vkGetInstanceProcAddr gipa,
              PFN_vkGetDeviceProcAddr gdpa);

    void shutdown();

    // (Re-)create the shadow image if needed. Idempotent for matching
    // (w, h, fmt). Returns false on any Vulkan failure.
    bool ensureShadow(uint32_t w, uint32_t h, VkFormat fmt);

    /*
     * Blit `imported` (dmabuf-backed VkImage, layout UNDEFINED on first
     * use, TRANSFER_SRC contents valid) into the shadow image. Waits on
     * `acquireSem` (may be VK_NULL_HANDLE for none). Signals
     * `releaseSyncobjFd` by importing it as an OPAQUE_FD binary
     * semaphore (TEMPORARY) and signaling that semaphore from the same
     * submit. Ownership of `releaseSyncobjFd` transfers in and is
     * always handed to the driver on success or closed on failure.
     *
     * Blocks the caller thread until the GPU copy completes
     * (vkWaitForFences). Performance OK for a wallpaper-rate pipeline;
     * revisit if we need overlap with Qt's render.
     *
     * Returns true on success. On false, shadow contents are
     * indeterminate; caller should skip rendering this frame.
     */
    bool blit(VkImage imported,
              uint32_t w, uint32_t h,
              VkSemaphore acquireSem,
              int releaseSyncobjFd);

    VkImage shadow() const { return m_shadowImage; }
    VkImageLayout shadowLayout() const {
        return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    bool initialized() const { return m_initialized; }

private:
    bool resolveFns();
    bool createCmdObjects();
    void destroyShadow();
    uint32_t pickMemoryType(uint32_t typeBits,
                            VkMemoryPropertyFlags req) const;

    VkInstance       m_instance{VK_NULL_HANDLE};
    VkPhysicalDevice m_phys{VK_NULL_HANDLE};
    VkDevice         m_device{VK_NULL_HANDLE};
    uint32_t         m_qfi{0};
    VkQueue          m_queue{VK_NULL_HANDLE};

    PFN_vkGetInstanceProcAddr m_gipa{nullptr};
    PFN_vkGetDeviceProcAddr   m_gdpa{nullptr};

    PFN_vkGetPhysicalDeviceMemoryProperties m_vkGetPhysicalDeviceMemoryProperties{nullptr};
    PFN_vkCreateImage                m_vkCreateImage{nullptr};
    PFN_vkDestroyImage               m_vkDestroyImage{nullptr};
    PFN_vkGetImageMemoryRequirements m_vkGetImageMemoryRequirements{nullptr};
    PFN_vkAllocateMemory             m_vkAllocateMemory{nullptr};
    PFN_vkFreeMemory                 m_vkFreeMemory{nullptr};
    PFN_vkBindImageMemory            m_vkBindImageMemory{nullptr};
    PFN_vkCreateCommandPool          m_vkCreateCommandPool{nullptr};
    PFN_vkDestroyCommandPool         m_vkDestroyCommandPool{nullptr};
    PFN_vkAllocateCommandBuffers     m_vkAllocateCommandBuffers{nullptr};
    PFN_vkResetCommandPool           m_vkResetCommandPool{nullptr};
    PFN_vkBeginCommandBuffer         m_vkBeginCommandBuffer{nullptr};
    PFN_vkEndCommandBuffer           m_vkEndCommandBuffer{nullptr};
    PFN_vkCmdPipelineBarrier         m_vkCmdPipelineBarrier{nullptr};
    PFN_vkCmdCopyImage               m_vkCmdCopyImage{nullptr};
    PFN_vkCreateFence                m_vkCreateFence{nullptr};
    PFN_vkDestroyFence               m_vkDestroyFence{nullptr};
    PFN_vkResetFences                m_vkResetFences{nullptr};
    PFN_vkWaitForFences              m_vkWaitForFences{nullptr};
    PFN_vkQueueSubmit                m_vkQueueSubmit{nullptr};
    PFN_vkDeviceWaitIdle             m_vkDeviceWaitIdle{nullptr};

    VkCommandPool   m_pool{VK_NULL_HANDLE};
    VkCommandBuffer m_cb{VK_NULL_HANDLE};
    VkFence         m_fence{VK_NULL_HANDLE};
    bool            m_fenceArmed{false};

    VkImage        m_shadowImage{VK_NULL_HANDLE};
    VkDeviceMemory m_shadowMem{VK_NULL_HANDLE};
    uint32_t       m_shadowW{0};
    uint32_t       m_shadowH{0};
    VkFormat       m_shadowFmt{VK_FORMAT_UNDEFINED};

    bool m_initialized{false};
};

#endif // WW_HAVE_VULKAN
