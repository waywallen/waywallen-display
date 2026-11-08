#pragma once

#include "FrameSlotRetirement.hpp"

#include <waywallen_display.h>

#include <QLoggingCategory>
#include <QMetaObject>
#include <QPointer>
#include <QRunnable>
#include <atomic>
#include <cstdint>
#include <memory>

Q_DECLARE_LOGGING_CATEGORY(lcWD)

class QOpenGLExtraFunctions;
class QQuickWindow;
class WaywallenDisplay;

class RenderSessionResources {
public:
    ~RenderSessionResources();

    void shutdown();
    bool ensureEglPresenter(QOpenGLExtraFunctions* functions);
    bool discardEglCandidate();
    bool commitEglCandidateRetaining();
    bool hasOutgoing() const;
    void retireOutgoing(const FrameSlotRetirement& retirement);
    bool collectRetired(int frameSlot, std::uint64_t frameSerial);

    waywallen_display_t*         display { nullptr };
    QPointer<QQuickWindow>       window;
    QMetaObject::Connection      invalidationConnection;
    WaywallenDisplay*            item { nullptr };
    std::atomic<bool>            callbacksEnabled { true };
    waywallen_egl_presenter_t*   eglPresenter { nullptr };
    waywallen_egl_presentable_t* eglOutgoing { nullptr };
    void*                        eglDisplay { nullptr };
#ifdef WW_HAVE_VULKAN
    waywallen_vulkan_presenter_t*   vkPresenter { nullptr };
    waywallen_vulkan_presentable_t* vkOutgoing { nullptr };
#endif

private:
    FrameSlotRetirementQueue<waywallen_egl_presentable_t*> eglRetired;
#ifdef WW_HAVE_VULKAN
    FrameSlotRetirementQueue<waywallen_vulkan_presentable_t*> vkRetired;
#endif
};

class RenderSessionCleanupJob : public QRunnable {
public:
    explicit RenderSessionCleanupJob(std::shared_ptr<RenderSessionResources> resources);
    void run() override;

private:
    std::shared_ptr<RenderSessionResources> m_resources;
};
