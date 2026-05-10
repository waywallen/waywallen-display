#include "WaywallenDisplay.hpp"

#include <waywallen_display.h>

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDebug>
#include <QLoggingCategory>
#include <QMouseEvent>
#include <QQuickGraphicsConfiguration>
#include <QQuickWindow>
#include <QRunnable>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QSGRendererInterface>
#include <QSGSimpleTextureNode>
#include <QWheelEvent>
#include <QtGui/qopenglcontext_platform.h>
#include <QtQuick/qsgtexture_platform.h>
#include <cstring>
#include <unistd.h>


Q_LOGGING_CATEGORY(lcWD, "waywallen.display")

namespace {
/* Tiny QRunnable adapter so cleanup() can post a render-thread shutdown
 * without keeping the QML item alive for the duration. The lib's
 * shutdown is bounded (close + 4 drain iterations + free), so no
 * watchdog needed here. */
class FnRunnable : public QRunnable {
public:
    explicit FnRunnable(std::function<void()> fn)
        : m_fn(std::move(fn)) { setAutoDelete(true); }
    void run() override { if (m_fn) m_fn(); }
private:
    std::function<void()> m_fn;
};
}  // namespace

// Linux input event codes — matches wlroots / Wayland convention so
// renderer plugins consuming forwarded events get a familiar enum.
static constexpr uint32_t WW_BTN_LEFT    = 0x110;
static constexpr uint32_t WW_BTN_RIGHT   = 0x111;
static constexpr uint32_t WW_BTN_MIDDLE  = 0x112;
static constexpr uint32_t WW_BTN_SIDE    = 0x113;
static constexpr uint32_t WW_BTN_EXTRA   = 0x114;

static uint32_t qtButtonToLinuxCode(Qt::MouseButton b) {
    switch (b) {
    case Qt::LeftButton:   return WW_BTN_LEFT;
    case Qt::RightButton:  return WW_BTN_RIGHT;
    case Qt::MiddleButton: return WW_BTN_MIDDLE;
    case Qt::BackButton:   return WW_BTN_SIDE;
    case Qt::ForwardButton:return WW_BTN_EXTRA;
    default:               return 0;
    }
}

// ---------------------------------------------------------------------------
// C library log → Qt log category bridge
// ---------------------------------------------------------------------------

static void qtLogBridge(waywallen_log_level_t level, const char *msg, void *) {
    switch (level) {
    case WAYWALLEN_LOG_DEBUG: qCDebug(lcWD, "%s", msg); break;
    case WAYWALLEN_LOG_INFO:  qCInfo(lcWD, "%s", msg); break;
    case WAYWALLEN_LOG_WARN:  qCWarning(lcWD, "%s", msg); break;
    case WAYWALLEN_LOG_ERROR: qCCritical(lcWD, "%s", msg); break;
    }
}

// ---------------------------------------------------------------------------
// C callback trampolines
// ---------------------------------------------------------------------------

void WaywallenDisplay::c_on_textures_ready(void *ud,
                                           const waywallen_textures_t *t) {
    auto *self = static_cast<WaywallenDisplay *>(ud);
    self->m_textureCount = t->count;
    self->m_texWidth = static_cast<int>(t->tex_width);
    self->m_texHeight = static_cast<int>(t->tex_height);

    if (t->backend == WAYWALLEN_BACKEND_EGL && t->egl_images) {
        qCInfo(lcWD, "textures ready: EGL, count=%u, size=%ux%u, fourcc=0x%x",
               t->count, t->tex_width, t->tex_height, t->fourcc);
        self->m_eglImagesValid = true;
        self->m_glTexturesCreated = false;
        self->m_glTextures.clear();
    } else if (t->backend == WAYWALLEN_BACKEND_VULKAN && t->vk_images) {
        qCInfo(lcWD, "textures ready: Vulkan, count=%u, size=%ux%u, fourcc=0x%x",
               t->count, t->tex_width, t->tex_height, t->fourcc);
        self->m_vkImagesValid = true;
        self->m_vkImages.resize(static_cast<int>(t->count));
        for (uint32_t i = 0; i < t->count; i++)
            self->m_vkImages[static_cast<int>(i)] = t->vk_images[i];
        self->m_vkFourcc = t->fourcc;
    } else {
        qCWarning(lcWD, "textures ready: backend=%d but no handles", t->backend);
        self->m_eglImagesValid = false;
        self->m_vkImagesValid = false;
    }
    self->setStreamState(Active);
}

void WaywallenDisplay::c_on_textures_releasing(void *ud,
                                               const waywallen_textures_t *t) {
    auto *self = static_cast<WaywallenDisplay *>(ud);
    Q_UNUSED(t);
    qCInfo(lcWD, "textures releasing");
    self->flushPendingRelease();

    // GL textures are owned by the C library (created via
    // waywallen_display_create_gl_texture); the library's cleanup
    // will delete them together with the EGLImages.
    self->m_eglImagesValid = false;
    self->m_glTexturesCreated = false;
    self->m_glTextures.clear();
    self->m_vkImagesValid = false;
    self->m_vkImages.clear();
    self->m_currentSlot = -1;
    self->m_textureCount = 0;

#ifdef WW_HAVE_VULKAN
    {
        QMutexLocker lk(&self->m_pendingMutex);
        if (self->m_pendingVk.valid && self->m_pendingVk.releaseSyncobjFd >= 0) {
            // Signal then close: closing alone leaves the daemon reaper
            // waiting the full BUCKET_TIMEOUT for this release_point.
            // Same fix the EGL teardown path (~line 277) already applies.
            (void)waywallen_display_signal_release_syncobj(
                self->m_pendingVk.releaseSyncobjFd);
        }
        self->m_pendingVk = PendingVkFrame{};
    }
    // Blitter teardown happens on the render thread (sceneGraphInvalidated
    // or cleanup()) — it owns Vulkan handles bound to a specific device.
#endif

    self->setStreamState(Inactive);
    self->update();
}

void WaywallenDisplay::c_on_config(void *ud, const waywallen_config_t *c) {
    auto *self = static_cast<WaywallenDisplay *>(ud);
    self->m_sourceRect = QRectF(
        static_cast<qreal>(c->source_rect.x),
        static_cast<qreal>(c->source_rect.y),
        static_cast<qreal>(c->source_rect.w),
        static_cast<qreal>(c->source_rect.h));
    self->m_destRect = QRectF(
        static_cast<qreal>(c->dest_rect.x),
        static_cast<qreal>(c->dest_rect.y),
        static_cast<qreal>(c->dest_rect.w),
        static_cast<qreal>(c->dest_rect.h));
    self->m_clearColor = QColor::fromRgbF(
        static_cast<qreal>(c->clear_color[0]),
        static_cast<qreal>(c->clear_color[1]),
        static_cast<qreal>(c->clear_color[2]),
        static_cast<qreal>(c->clear_color[3]));
    emit self->clearColorChanged();
    // Schedule a repaint so a fillmode/align change applied while no
    // new frame is in flight still becomes visible — updatePaintNode
    // re-reads m_sourceRect/m_destRect.
    self->update();
}

void WaywallenDisplay::c_on_frame_ready(void *ud,
                                        const waywallen_frame_t *f) {
    auto *self = static_cast<WaywallenDisplay *>(ud);
    // flushPendingRelease signals the *prior* frame's release_syncobj
    // (EGL path) so that producer's wait at that release_point doesn't
    // time out. Vulkan signals from the blitter once the GPU copy is
    // done; the EGL path approximates "done with prior frame" by
    // signaling when the next frame_ready arrives.
    self->flushPendingRelease();
    self->m_framesReceived++;
    self->m_currentSlot = static_cast<int>(f->buffer_index);

#ifdef WW_HAVE_VULKAN
    if (self->m_activeBackend == BackendVulkan) {
        // Hand-off to render thread. If a prior frame is still queued
        // (render thread didn't blit it yet), drop it: signal its
        // release_syncobj so the daemon reaper closes the bucket
        // immediately instead of waiting BUCKET_TIMEOUT (500ms). The
        // buffer is "released" — we never read it.
        QMutexLocker lk(&self->m_pendingMutex);
        if (self->m_pendingVk.valid && self->m_pendingVk.releaseSyncobjFd >= 0) {
            (void)waywallen_display_signal_release_syncobj(
                self->m_pendingVk.releaseSyncobjFd);
        }
        self->m_pendingVk.valid = true;
        self->m_pendingVk.slot = static_cast<int>(f->buffer_index);
        self->m_pendingVk.acquireSem = f->vk_acquire_semaphore;
        self->m_pendingVk.releaseSyncobjFd = f->release_syncobj_fd;
    } else
#endif
    if (self->m_activeBackend == BackendEGL) {
        // Capture the fd; flushPendingRelease on the *next* frame_ready
        // signals it. If a prior fd was somehow still pending (frames
        // arrived faster than flushPendingRelease ran), signal that one
        // first instead of leaking it.
        if (self->m_pendingEglReleaseSyncobjFd >= 0) {
            (void)waywallen_display_signal_release_syncobj(
                self->m_pendingEglReleaseSyncobjFd);
            self->m_pendingEglReleaseSyncobjFd = -1;
        }
        self->m_pendingEglReleaseSyncobjFd = f->release_syncobj_fd;
    } else if (f->release_syncobj_fd >= 0) {
        // No active backend yet (textures haven't arrived). Signal +
        // close so the daemon reaper closes the bucket immediately
        // rather than waiting BUCKET_TIMEOUT (500ms) and force-flushing.
        (void)waywallen_display_signal_release_syncobj(
            f->release_syncobj_fd);
    }

    emit self->framesReceivedChanged();
    self->update();
}

void WaywallenDisplay::c_on_disconnected(void *ud, int err,
                                         const char *msg) {
    auto *self = static_cast<WaywallenDisplay *>(ud);
    // If m_display was nulled (e.g. by sceneGraphInvalidated on the
    // render thread), skip — we're already tearing down.
    if (!self->m_display) return;
    const auto reason = static_cast<DisconnectReason>(
        waywallen_display_last_disconnect_reason(self->m_display));
    const QString message = QString::fromUtf8(
        waywallen_display_last_disconnect_message(self->m_display));
    if (self->m_lastReason != reason || self->m_lastMessage != message) {
        self->m_lastReason = reason;
        self->m_lastMessage = message;
        emit self->lastDisconnectChanged();
    }
    self->handleDisconnect(err, msg);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

WaywallenDisplay::WaywallenDisplay(QQuickItem *parent)
    : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
    waywallen_display_set_log_callback(qtLogBridge, nullptr);

    m_updateSizeTimer.setSingleShot(true);
    m_updateSizeTimer.setInterval(100);
    connect(&m_updateSizeTimer, &QTimer::timeout,
            this, &WaywallenDisplay::pushSizeUpdate);
}

WaywallenDisplay::~WaywallenDisplay() {
    // cleanup() may already have been called by sceneGraphInvalidated.
    // It's safe to call again — it checks m_display for null.
    cleanup();
}

// Render-thread teardown. Called either:
//   - from sceneGraphInvalidated lambda (render thread, SG dying), or
//   - from the FnRunnable cleanup() schedules onto the render thread.
// Idempotent. Owns the GPU-touching shutdown: lib's close+drain+free
// (lib's drain processes the pending-destroy list it deferred from the
// I/O thread) and the blitter shutdown.
void WaywallenDisplay::renderThreadFinalize() {
    auto *d = m_display;
    m_display = nullptr;
    if (d) waywallen_display_shutdown(d);
#ifdef WW_HAVE_VULKAN
    if (m_vkBlitterInited) {
        ww_vk_blitter_shutdown(&m_vkBlitter);
        m_vkBlitterInited = false;
    }
#endif
    /* EGL shadow: GL objects on the QSG render context. Same render
     * thread that's calling us, so the GL ops in destroyEglShadow
     * land on the right context. */
    destroyEglShadow();
}

void WaywallenDisplay::cleanup() {
    if (m_filterInstalled && window()) {
        window()->removeEventFilter(this);
    }
    m_filterInstalled = false;

    /* Notifiers live on GUI thread — direct delete. cleanup() always
     * runs on GUI thread (called from ~WaywallenDisplay or
     * handleDisconnect). The sceneGraphInvalidated path uses a
     * different teardown route via deleteLater. */
    delete m_notifier;
    m_notifier = nullptr;
    delete m_notifierWrite;
    m_notifierWrite = nullptr;

    /* Drop any unsignaled release_syncobj fds before tearing down the
     * lib handle — signaling (vs. just closing) lets the daemon's
     * reaper observe the release immediately instead of waiting the
     * full BUCKET_TIMEOUT for the slot. */
    if (m_pendingEglReleaseSyncobjFd >= 0) {
        (void)waywallen_display_signal_release_syncobj(
            m_pendingEglReleaseSyncobjFd);
        m_pendingEglReleaseSyncobjFd = -1;
    }
#ifdef WW_HAVE_VULKAN
    {
        QMutexLocker lk(&m_pendingMutex);
        if (m_pendingVk.valid && m_pendingVk.releaseSyncobjFd >= 0) {
            (void)waywallen_display_signal_release_syncobj(
                m_pendingVk.releaseSyncobjFd);
        }
        m_pendingVk = PendingVkFrame{};
    }
#endif

    if (m_display) {
        QQuickWindow *w = window();
        if (w && w->isSceneGraphInitialized() && w->isExposed()) {
            /* Hop ownership to the render thread; it will run the
             * full close+drain+free sequence plus blitter shutdown
             * via renderThreadFinalize. NoStage posts a WMJobEvent
             * straight to the render-thread event loop — no
             * polishAndSync cooperation needed, so this works even
             * if the GUI thread is busy. We do NOT block waiting
             * for it: the lib's shutdown is bounded and GUI-thread
             * stalls on display teardown are user-visible jank. */
            auto *self = this;
            w->scheduleRenderJob(new FnRunnable([self]() {
                self->renderThreadFinalize();
            }), QQuickWindow::NoStage);
        } else {
            /* No render thread to safely drain on — Plasma extension
             * likely tearing the whole window down. Close the socket
             * (any thread, idempotent) so the daemon sees us go away;
             * the GPU resources leak until process exit. We can't
             * call free() — it would abort on the pending pools that
             * close() just enqueued. */
            qCCritical(lcWD,
                "no render context for shutdown; closing socket and "
                "leaking GPU resources (process exit will reclaim)");
            waywallen_display_close(m_display);
            m_display = nullptr;
#ifdef WW_HAVE_VULKAN
            if (m_vkBlitterInited) {
                std::memset(&m_vkBlitter, 0, sizeof(m_vkBlitter));
                m_vkBlitterInited = false;
            }
#endif
        }
    }

    m_updateSizeTimer.stop();
    m_lastPushedWidth  = -1;
    m_lastPushedHeight = -1;

    m_eglImagesValid = false;
    m_glTexturesCreated = false;
    m_glTextures.clear();
    m_vkImagesValid = false;
    m_vkImages.clear();
    m_currentSlot = -1;
    m_textureCount = 0;
    m_activeBackend = BackendNone;

    if (m_displayId != 0) {
        m_displayId = 0;
        emit displayIdChanged();
    }
}

// ---------------------------------------------------------------------------
// Properties
// ---------------------------------------------------------------------------

void WaywallenDisplay::setSocketPath(const QString &path) {
    if (m_socketPath == path) return;
    m_socketPath = path;
    emit socketPathChanged();
}

void WaywallenDisplay::setDisplayName(const QString &name) {
    if (m_displayName == name) return;
    m_displayName = name;
    emit displayNameChanged();
}

void WaywallenDisplay::setInstanceId(const QString &id) {
    if (m_instanceId == id) return;
    m_instanceId = id;
    emit instanceIdChanged();
}

void WaywallenDisplay::setDisplayWidth(int w) {
    if (m_displayWidth == w) return;
    m_displayWidth = w;
    emit displayWidthChanged();
    if (m_display) m_updateSizeTimer.start();
}

void WaywallenDisplay::setDisplayHeight(int h) {
    if (m_displayHeight == h) return;
    m_displayHeight = h;
    emit displayHeightChanged();
    if (m_display) m_updateSizeTimer.start();
}

void WaywallenDisplay::pushSizeUpdate() {
    if (!m_display) return;
    if (waywallen_display_conn_state(m_display) != WAYWALLEN_CONN_CONNECTED) {
        // Drop silently — once the handshake completes, register_display
        // already carried the latest dims via begin_connect.
        return;
    }
    if (m_displayWidth <= 0 || m_displayHeight <= 0) return;
    if (m_lastPushedWidth == m_displayWidth &&
        m_lastPushedHeight == m_displayHeight) {
        return;
    }
    int rc = waywallen_display_update_size(
        m_display,
        static_cast<uint32_t>(m_displayWidth),
        static_cast<uint32_t>(m_displayHeight));
    if (rc != 0) {
        qCWarning(lcWD, "update_size(%d, %d) failed: %d",
                  m_displayWidth, m_displayHeight, rc);
        return;
    }
    m_lastPushedWidth = m_displayWidth;
    m_lastPushedHeight = m_displayHeight;
    armWriteNotifier();
}

void WaywallenDisplay::setAutoReconnect(bool enabled) {
    if (m_autoReconnect == enabled) return;
    m_autoReconnect = enabled;
    emit autoReconnectChanged();
}

void WaywallenDisplay::setMouseForwardEnabled(bool enabled) {
    if (m_mouseForwardEnabled == enabled) return;
    m_mouseForwardEnabled = enabled;
    if (window()) {
        if (enabled && !m_filterInstalled) {
            window()->installEventFilter(this);
            m_filterInstalled = true;
        } else if (!enabled && m_filterInstalled) {
            window()->removeEventFilter(this);
            m_filterInstalled = false;
        }
    }
    emit mouseForwardEnabledChanged();
}

bool WaywallenDisplay::eventFilter(QObject *obj, QEvent *ev) {
    if (!m_mouseForwardEnabled || obj != window() || !m_display) {
        return false;
    }
    if (waywallen_display_conn_state(m_display) != WAYWALLEN_CONN_CONNECTED) {
        return false;
    }

    const QRectF bounds = boundingRect();
    if (bounds.width() <= 0 || bounds.height() <= 0) return false;

    auto toSurface = [&](const QPointF &scenePos, float &px, float &py) -> bool {
        const QPointF inItem = mapFromScene(scenePos);
        if (!bounds.contains(inItem)) return false;
        const float sx = float(m_displayWidth)  / float(bounds.width());
        const float sy = float(m_displayHeight) / float(bounds.height());
        px = float(inItem.x()) * sx;
        py = float(inItem.y()) * sy;
        return true;
    };

    switch (ev->type()) {
    case QEvent::MouseMove: {
        auto *me = static_cast<QMouseEvent *>(ev);
        float px, py;
        if (!toSurface(me->scenePosition(), px, py)) return false;
        const uint64_t ts = uint64_t(me->timestamp()) * 1000ull;
        (void)waywallen_display_send_pointer_motion(
            m_display, px, py, ts, 0);
        break;
    }
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease: {
        auto *me = static_cast<QMouseEvent *>(ev);
        float px, py;
        if (!toSurface(me->scenePosition(), px, py)) return false;
        const uint32_t code = qtButtonToLinuxCode(me->button());
        if (code == 0) return false;
        const uint64_t ts = uint64_t(me->timestamp()) * 1000ull;
        const auto state = (ev->type() == QEvent::MouseButtonPress)
                               ? WAYWALLEN_BUTTON_PRESSED
                               : WAYWALLEN_BUTTON_RELEASED;
        (void)waywallen_display_send_pointer_button(
            m_display, px, py, code, state, ts, 0);
        break;
    }
    case QEvent::Wheel: {
        auto *we = static_cast<QWheelEvent *>(ev);
        float px, py;
        if (!toSurface(we->position(), px, py)) return false;
        const QPoint angle = we->angleDelta();
        const float dx = float(angle.x()) / 120.0f;
        const float dy = float(angle.y()) / 120.0f;
        if (dx == 0.0f && dy == 0.0f) return false;
        const uint64_t ts = uint64_t(we->timestamp()) * 1000ull;
        (void)waywallen_display_send_pointer_axis(
            m_display, px, py, dx, dy, WAYWALLEN_AXIS_WHEEL, ts, 0);
        break;
    }
    default:
        break;
    }
    armWriteNotifier();
    return false;
}

void WaywallenDisplay::setConnState(ConnState s) {
    if (m_connState == s) return;
    m_connState = s;
    emit connStateChanged();
}

void WaywallenDisplay::setStreamState(StreamState s) {
    if (m_streamState == s) return;
    m_streamState = s;
    emit streamStateChanged();
}

// ---------------------------------------------------------------------------
// Backend binding helpers
// ---------------------------------------------------------------------------

// Trampoline so the C library can call QOpenGLContext::getProcAddress
// without needing a Qt include. The QOpenGLContext is process-scoped
// for our purposes (Qt's QSG renderer reuses a single context); we
// stash it here when bindEglBackend runs.
static QOpenGLContext *s_qtGlCtxForProcAddr = nullptr;
static void *qtEglGetProcAddress(const char *name) {
    if (!s_qtGlCtxForProcAddr) return nullptr;
    auto fn = s_qtGlCtxForProcAddr->getProcAddress(name);
    return reinterpret_cast<void *>(fn);
}

bool WaywallenDisplay::bindEglBackend() {
    auto *rif = window()->rendererInterface();
    auto *glCtx = static_cast<QOpenGLContext *>(
        rif ? rif->getResource(window(),
                               QSGRendererInterface::OpenGLContextResource)
            : nullptr);
    if (!glCtx) {
        qCWarning(lcWD, "OpenGL API but no QOpenGLContext");
        return false;
    }

    auto *eglIface = glCtx->nativeInterface<QNativeInterface::QEGLContext>();
    if (!eglIface) {
        qCWarning(lcWD, "OpenGL context has no EGL interface (GLX?)");
        return false;
    }

    s_qtGlCtxForProcAddr = glCtx;

    waywallen_egl_ctx_t egl_ctx {};
    egl_ctx.egl_display = eglIface->display();
    egl_ctx.get_proc_address = &qtEglGetProcAddress;
    int rc = waywallen_display_bind_egl(m_display, &egl_ctx);
    if (rc != WAYWALLEN_OK) {
        qCWarning(lcWD, "bind_egl failed: %d", rc);
        return false;
    }
    qCInfo(lcWD, "bound EGL backend, display=%p",
           static_cast<void *>(egl_ctx.egl_display));
    return true;
}

bool WaywallenDisplay::bindVulkanBackend() {
    auto *qvkInst = window()->vulkanInstance();
    if (!qvkInst || !qvkInst->isValid()) {
        qCWarning(lcWD, "no valid QVulkanInstance on window");
        return false;
    }

    auto *rif = window()->rendererInterface();
    if (!rif) return false;

    // VulkanInstanceResource returns VkInstance (not QVulkanInstance*).
    // Qt's getResource returns a pointer TO the Vulkan handle, not the
    // handle itself. Dereference to get the actual VkPhysicalDevice / VkDevice.
    auto *pPhysDev = static_cast<VkPhysicalDevice *>(
        rif->getResource(window(), QSGRendererInterface::PhysicalDeviceResource));
    auto *pDevice = static_cast<VkDevice *>(
        rif->getResource(window(), QSGRendererInterface::DeviceResource));

    if (!pPhysDev || !pDevice) {
        qCWarning(lcWD, "Vulkan API but missing resources "
                  "(phys=%p dev=%p)",
                  static_cast<void *>(pPhysDev),
                  static_cast<void *>(pDevice));
        return false;
    }

    VkPhysicalDevice physDev = *pPhysDev;
    VkDevice device = *pDevice;

#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    auto *qfip = static_cast<uint32_t *>(rif->getResource(window(),
        QSGRendererInterface::GraphicsQueueFamilyIndexResource));
    uint32_t qfi = qfip ? *qfip : 0;
#else
    uint32_t qfi = 0;
#endif

    VkInstance vkInstance = qvkInst->vkInstance();

    // Resolve the global vkGetInstanceProcAddr.
    auto rawGIPA = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        qvkInst->getInstanceProcAddr("vkGetInstanceProcAddr"));
    if (!rawGIPA) {
        qCWarning(lcWD, "failed to resolve vkGetInstanceProcAddr");
        return false;
    }

    waywallen_vk_ctx_t vk_ctx {};
    vk_ctx.instance = reinterpret_cast<void *>(vkInstance);
    vk_ctx.physical_device = reinterpret_cast<void *>(physDev);
    vk_ctx.device = reinterpret_cast<void *>(device);
    vk_ctx.queue_family_index = qfi;
    vk_ctx.vk_get_instance_proc_addr =
        reinterpret_cast<void *(*)(void *, const char *)>(rawGIPA);

    int rc = waywallen_display_bind_vulkan(m_display, &vk_ctx);
    if (rc != WAYWALLEN_OK) {
        qCWarning(lcWD, "bind_vulkan failed: %d", rc);
        return false;
    }

#ifdef WW_HAVE_VULKAN
    m_vkInstance = reinterpret_cast<void *>(vkInstance);
    m_vkPhys = reinterpret_cast<void *>(physDev);
    m_vkDevice = reinterpret_cast<void *>(device);
    m_vkQfi = qfi;
    m_vkGipa = reinterpret_cast<void *(*)(void *, const char *)>(rawGIPA);

    // VkQueue for our blit submits. Use the same queue Qt uses so
    // submission order on a single queue gives us implicit ordering
    // against Qt's later sample. CommandQueueResource returns a
    // pointer-to-VkQueue, same convention as Device/PhysicalDevice
    // resources.
    auto *pQueue = static_cast<VkQueue *>(rif->getResource(window(),
        QSGRendererInterface::CommandQueueResource));
    if (pQueue && *pQueue != VK_NULL_HANDLE) {
        m_vkQueue = reinterpret_cast<void *>(*pQueue);
    } else {
        // Fallback: ask the device for the family's queue 0. Most Qt
        // setups use index 0; if Qt uses a different index, this races
        // with Qt's own submits and we'd need a semaphore handshake.
        auto vkGetDeviceQueue = reinterpret_cast<PFN_vkGetDeviceQueue>(
            qvkInst->getInstanceProcAddr("vkGetDeviceQueue"));
        if (vkGetDeviceQueue) {
            VkQueue q = VK_NULL_HANDLE;
            vkGetDeviceQueue(device, qfi, 0, &q);
            m_vkQueue = reinterpret_cast<void *>(q);
        }
        qCInfo(lcWD, "Qt did not expose CommandQueueResource; using "
                     "vkGetDeviceQueue(qfi=%u, idx=0)=%p",
               qfi, m_vkQueue);
    }
#endif

    qCInfo(lcWD, "bound Vulkan backend, device=%p",
           reinterpret_cast<void *>(device));
    return true;
}

// ---------------------------------------------------------------------------
// Connection + reconnect
// ---------------------------------------------------------------------------

void WaywallenDisplay::componentComplete() {
    QQuickItem::componentComplete();
    setupDBusWatcher();
    if (window()) {
        onWindowReady();
    } else {
        connect(this, &QQuickItem::windowChanged,
                this, &WaywallenDisplay::onWindowReady);
    }
}

void WaywallenDisplay::setupDBusWatcher() {
    // Optional: if there is no session bus (headless, TTY, container
    // without DBus), the exponential-backoff reconnect stays as the
    // fallback and we simply don't get the fast path.
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        qCInfo(lcWD, "session bus unavailable — DBus reconnect fast-path disabled");
        return;
    }

    // NameOwnerChanged — fires when the daemon claims or releases
    // org.waywallen.waywallen.Daemon. The "new owner is non-empty" case means the
    // daemon just (re)appeared; reconnect now instead of waiting for the
    // local backoff timer.
    const bool okNoc = bus.connect(
        QStringLiteral("org.freedesktop.DBus"),
        QStringLiteral("/org/freedesktop/DBus"),
        QStringLiteral("org.freedesktop.DBus"),
        QStringLiteral("NameOwnerChanged"),
        QStringLiteral("sss"),
        this,
        SLOT(onDaemonNameOwnerChanged(QString, QString, QString)));
    if (!okNoc) {
        qCWarning(lcWD, "failed to subscribe to NameOwnerChanged");
    }

    // Also subscribe directly to the daemon's Ready signal as a second
    // cue; on some timings NameOwnerChanged is delivered before the
    // server is fully serving requests, and Ready is emitted only once
    // the daemon is actually ready. Either trigger does the same thing.
    const bool okReady = bus.connect(
        QStringLiteral("org.waywallen.waywallen.Daemon"),
        QStringLiteral("/org/waywallen/waywallen/Daemon"),
        QStringLiteral("org.waywallen.waywallen.Daemon1"),
        QStringLiteral("Ready"),
        this,
        SLOT(onDaemonReadySignal()));
    if (!okReady) {
        qCWarning(lcWD, "failed to subscribe to org.waywallen.waywallen.Daemon Ready");
    }

    qCInfo(lcWD, "DBus reconnect fast-path armed");
}

void WaywallenDisplay::onDaemonNameOwnerChanged(const QString &name,
                                                const QString &oldOwner,
                                                const QString &newOwner) {
    Q_UNUSED(oldOwner);
    if (name != QStringLiteral("org.waywallen.waywallen.Daemon")) return;
    if (newOwner.isEmpty()) return;  // daemon vanished — UDS disconnect handles it
    qCInfo(lcWD, "daemon appeared on session bus — requesting reconnect");
    requestReconnect();
}

void WaywallenDisplay::onDaemonReadySignal() {
    qCInfo(lcWD, "daemon Ready signal — requesting reconnect");
    requestReconnect();
}

void WaywallenDisplay::requestReconnect() {
    if (m_connState == Connected || m_connState == Handshaking) return;
    if (!m_autoReconnect) return;
    tryConnect();
}

void WaywallenDisplay::onWindowReady() {
    if (!window()) return;

    if (m_mouseForwardEnabled) {
        // installEventFilter is idempotent on the same (target, filter)
        // pair; safe to call again if windowChanged refires.
        window()->installEventFilter(this);
        m_filterInstalled = true;
    }

    if (m_display) return;

    // sceneGraphInvalidated: SG is being torn down (window closing,
    // renderer reset, etc). Fires on render thread with DirectConnection.
    // We have to release GPU resources NOW — by the time this returns
    // Qt will start destroying its VkDevice / GL context.
    connect(window(), &QQuickWindow::sceneGraphInvalidated, this,
            [this]() {
                qCInfo(lcWD, "sceneGraphInvalidated: releasing GPU resources");
#ifdef WW_HAVE_VULKAN
                {
                    QMutexLocker lk(&m_pendingMutex);
                    if (m_pendingVk.valid && m_pendingVk.releaseSyncobjFd >= 0) {
                        (void)waywallen_display_signal_release_syncobj(
                            m_pendingVk.releaseSyncobjFd);
                    }
                    m_pendingVk = PendingVkFrame{};
                }
#endif
                renderThreadFinalize();
                // Notifiers live on GUI thread; this lambda is on render
                // thread. deleteLater is documented thread-safe; it
                // posts a destruction event back to the notifier's GUI
                // thread. QPointer ensures we don't double-free if
                // cleanup() also runs in parallel.
                if (m_notifier)      m_notifier->deleteLater();
                if (m_notifierWrite) m_notifierWrite->deleteLater();
                m_notifier.clear();
                m_notifierWrite.clear();
                m_eglImagesValid = false;
                m_glTexturesCreated = false;
                m_glTextures.clear();
                m_vkImagesValid = false;
                m_vkImages.clear();
                m_currentSlot = -1;
                m_textureCount = 0;
                m_activeBackend = BackendNone;
            },
            Qt::DirectConnection);

    if (!window()->isSceneGraphInitialized()) {
        // Inject Vulkan device extensions needed for DMA-BUF import
        // before the scene graph creates the VkDevice.
        auto config = window()->graphicsConfiguration();
        config.setDeviceExtensions({
            "VK_KHR_external_memory",
            "VK_KHR_external_memory_fd",
            "VK_EXT_external_memory_dma_buf",
            "VK_EXT_image_drm_format_modifier",
            "VK_KHR_external_semaphore",
            "VK_KHR_external_semaphore_fd",
        });
        window()->setGraphicsConfiguration(config);
        qCInfo(lcWD, "requested DMA-BUF Vulkan device extensions");

        connect(window(), &QQuickWindow::sceneGraphInitialized,
                this, &WaywallenDisplay::tryConnect,
                Qt::UniqueConnection);
        return;
    }
    tryConnect();
}

void WaywallenDisplay::tryConnect() {
    if (m_display) return;
    setConnState(Connecting);

    waywallen_display_callbacks_t cb {};
    cb.on_textures_ready = c_on_textures_ready;
    cb.on_textures_releasing = c_on_textures_releasing;
    cb.on_config = c_on_config;
    cb.on_frame_ready = c_on_frame_ready;
    cb.on_disconnected = c_on_disconnected;
    cb.user_data = this;

    m_display = waywallen_display_new(&cb);
    if (!m_display) {
        qCWarning(lcWD, "waywallen_display_new() failed");
        setConnState(Error);
        // No internal retry — DBus NameOwnerChanged / Ready will drive
        // the next attempt when the daemon (re-)appears.
        return;
    }

    // cleanup() removed the event filter on the prior session's
    // teardown; reinstall it now so mouse events resume forwarding
    // after a daemon-restart reconnect. Idempotent — Qt deduplicates
    // (target, filter) pairs.
    if (m_mouseForwardEnabled && !m_filterInstalled && window()) {
        window()->installEventFilter(this);
        m_filterInstalled = true;
    }

    // Auto-detect Qt's graphics API and bind the matching backend.
    m_activeBackend = BackendNone;
    if (window()) {
        auto *rif = window()->rendererInterface();
        if (rif) {
            auto api = rif->graphicsApi();
            qCInfo(lcWD, "Qt graphics API: %d", int(api));

            if (api == QSGRendererInterface::OpenGL) {
                if (bindEglBackend())
                    m_activeBackend = BackendEGL;
            } else if (api == QSGRendererInterface::Vulkan) {
                if (bindVulkanBackend())
                    m_activeBackend = BackendVulkan;
            } else {
                qCWarning(lcWD, "unsupported graphics API: %d", int(api));
            }
        }
    }

    if (m_activeBackend == BackendNone) {
        qCWarning(lcWD, "no backend bound — textures will not be imported");
    }

    const QByteArray sockPath = m_socketPath.toUtf8();
    const QByteArray name = m_displayName.toUtf8();
    const QByteArray instanceId = m_instanceId.toUtf8();
    int rc = waywallen_display_begin_connect(
        m_display,
        sockPath.isEmpty() ? nullptr : sockPath.constData(),
        name.constData(),
        instanceId.isEmpty() ? nullptr : instanceId.constData(),
        static_cast<uint32_t>(m_displayWidth),
        static_cast<uint32_t>(m_displayHeight),
        60000);

    if (rc != WAYWALLEN_OK) {
        qCWarning(lcWD, "begin_connect failed: %d (waiting for daemon DBus signal)", rc);
        waywallen_display_free(m_display);
        m_display = nullptr;
        setConnState(Disconnected);
        return;
    }

    // begin_connect carries these dims to the daemon as part of
    // register_display, so seed the dedupe so a same-size resize
    // post-handshake is a no-op.
    m_lastPushedWidth  = m_displayWidth;
    m_lastPushedHeight = m_displayHeight;

    int fd = waywallen_display_get_fd(m_display);
    if (fd < 0) {
        qCWarning(lcWD, "begin_connect returned no fd");
        waywallen_display_free(m_display);
        m_display = nullptr;
        setConnState(Disconnected);
        return;
    }

    setConnState(Handshaking);

    // Two notifiers drive the async handshake until READY. Read fires on
    // POLLIN (welcome / display_accepted), Write on POLLOUT (initial
    // connect completion + hello / register_display sends). The state
    // machine in advance_handshake decides which one to enable next.
    m_notifier      = new QSocketNotifier(fd, QSocketNotifier::Read,  this);
    m_notifierWrite = new QSocketNotifier(fd, QSocketNotifier::Write, this);
    connect(m_notifier,      &QSocketNotifier::activated,
            this, &WaywallenDisplay::onHandshakeIO);
    connect(m_notifierWrite, &QSocketNotifier::activated,
            this, &WaywallenDisplay::onHandshakeIO);

    // Initial arming: write is needed for either completing connect or
    // sending hello; read is armed too in case the kernel completed
    // connect already and a welcome arrives immediately.
    m_notifier->setEnabled(true);
    m_notifierWrite->setEnabled(true);
}

void WaywallenDisplay::onHandshakeIO() {
    if (!m_display) return;
    int rc = waywallen_display_advance_handshake(m_display);
    if (rc == WAYWALLEN_HS_DONE) {
        qCInfo(lcWD, "handshake complete");
        // Repurpose both notifiers post-handshake:
        //   read  → onSocketReadable (drives lib's dispatch)
        //   write → onSocketWritable (drives lib's outbox flush)
        // The write notifier stays disabled by default; we only arm
        // it when the outbox has unsent bytes, via wants_writable
        // probes after each enqueue. This keeps non-blocking sends
        // from spinning on EAGAIN.
        if (m_notifier) {
            disconnect(m_notifier, &QSocketNotifier::activated,
                       this, &WaywallenDisplay::onHandshakeIO);
            connect(m_notifier, &QSocketNotifier::activated,
                    this, &WaywallenDisplay::onSocketReadable);
            m_notifier->setEnabled(true);
        }
        if (m_notifierWrite) {
            disconnect(m_notifierWrite, &QSocketNotifier::activated,
                       this, &WaywallenDisplay::onHandshakeIO);
            connect(m_notifierWrite, &QSocketNotifier::activated,
                    this, &WaywallenDisplay::onSocketWritable);
            m_notifierWrite->setEnabled(
                waywallen_display_wants_writable(m_display));
        }
        setStreamState(Inactive);
        setConnState(Connected);
        const auto newId =
            qulonglong(waywallen_display_get_display_id(m_display));
        if (m_displayId != newId) {
            m_displayId = newId;
            emit displayIdChanged();
        }
        if (m_lastReason != None || !m_lastMessage.isEmpty()) {
            m_lastReason = None;
            m_lastMessage.clear();
            emit lastDisconnectChanged();
        }
        // Window may have resized while the handshake was in flight;
        // reconcile by pushing if the current dims drifted from what
        // begin_connect carried.
        if (m_displayWidth  != m_lastPushedWidth ||
            m_displayHeight != m_lastPushedHeight) {
            m_updateSizeTimer.start();
        }
        return;
    }
    if (rc < 0) {
        handleDisconnect(rc, "handshake");
        return;
    }
    // NEED_READ / NEED_WRITE: arm the matching notifier, idle the other.
    if (m_notifier)      m_notifier     ->setEnabled(rc == WAYWALLEN_HS_NEED_READ);
    if (m_notifierWrite) m_notifierWrite->setEnabled(rc == WAYWALLEN_HS_NEED_WRITE);
}

void WaywallenDisplay::onSocketReadable() {
    if (!m_display) return;
    // EGLImage creation (EGL path) and VkImage import (Vulkan path)
    // do not require a GL context. GL textures are created lazily
    // in updatePaintNode on the render thread.
    waywallen_display_dispatch(m_display);
    // dispatch may have queued an outgoing message (e.g. unbind_done
    // from handle_unbind). Re-arm POLLOUT if anything stayed queued.
    armWriteNotifier();
}

void WaywallenDisplay::onSocketWritable() {
    if (!m_display) return;
    waywallen_display_handle_writable(m_display);
    armWriteNotifier();
}

void WaywallenDisplay::armWriteNotifier() {
    if (m_notifierWrite && m_display) {
        m_notifierWrite->setEnabled(
            waywallen_display_wants_writable(m_display));
    }
}

void WaywallenDisplay::flushPendingRelease() {
    // EGL path: signal the prior frame's release_syncobj. The helper
    // imports the fd as a syncobj handle on this process's DRM device,
    // SIGNALs it, and closes the fd in all paths. The kernel-side
    // syncobj stays alive (the daemon still holds a handle ref); the
    // signal is what the daemon's reaper is waiting on.
    if (m_pendingEglReleaseSyncobjFd >= 0) {
        int rc = waywallen_display_signal_release_syncobj(
            m_pendingEglReleaseSyncobjFd);
        if (rc != WAYWALLEN_OK) {
            qCWarning(lcWD,
                "EGL: signal_release_syncobj failed: %d "
                "(daemon will time out the slot)", rc);
        }
        m_pendingEglReleaseSyncobjFd = -1;
    }
}

void WaywallenDisplay::handleDisconnect(int errCode, const char *msg) {
    qCWarning(lcWD, "disconnected (err=%d msg=%s) — waiting for daemon DBus signal",
              errCode, msg ? msg : "(null)");
    cleanup();
    setConnState(Disconnected);
    setStreamState(Inactive);
    update();
    // No retry timer — wait for org.waywallen.waywallen.Daemon NameOwnerChanged
    // / Ready signals to drive the next attempt (see setupDBusWatcher).
}

// ---------------------------------------------------------------------------
// EGL: deferred GL texture creation (called on render thread)
// ---------------------------------------------------------------------------

void WaywallenDisplay::ensureGlTextures() {
    if (m_glTexturesCreated || !m_eglImagesValid || !m_display) return;

    m_glTextures.resize(static_cast<int>(m_textureCount));
    bool ok = true;
    for (uint32_t i = 0; i < m_textureCount; i++) {
        uint32_t tex = 0;
        int rc = waywallen_display_create_gl_texture(m_display, i, &tex);
        if (rc != WAYWALLEN_OK) {
            qCWarning(lcWD, "create_gl_texture[%u] failed: %d", i, rc);
            ok = false;
            break;
        }
        m_glTextures[static_cast<int>(i)] = tex;
    }

    if (ok) {
        m_glTexturesCreated = true;
        qCInfo(lcWD, "created %u GL textures on render thread", m_textureCount);
    } else {
        m_glTextures.clear();
    }
}

bool WaywallenDisplay::blitEglShadow(int slot) {
    if (slot < 0 || slot >= m_glTextures.size()) return false;
    auto *ctx = QOpenGLContext::currentContext();
    if (!ctx) return false;
    auto *gl = ctx->extraFunctions();
    if (!gl) return false;

    const int w = m_texWidth;
    const int h = m_texHeight;
    if (w <= 0 || h <= 0) return false;

    /* Save Qt's current FBO binding so we don't disturb its renderer
     * state. We restore both READ and DRAW targets at the end. */
    GLint prevDraw = 0, prevRead = 0;
    gl->glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDraw);
    gl->glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevRead);

    /* Lazy-allocate / resize the shadow texture. RGBA8 storage —
     * matches the 8 fourccs in src/drm_fourcc_internal.h. */
    if (m_eglShadowTex == 0) {
        gl->glGenTextures(1, &m_eglShadowTex);
        gl->glBindTexture(GL_TEXTURE_2D, m_eglShadowTex);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    if (m_eglShadowW != w || m_eglShadowH != h) {
        gl->glBindTexture(GL_TEXTURE_2D, m_eglShadowTex);
        gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        m_eglShadowW = w;
        m_eglShadowH = h;
        m_eglShadowHasContent = false;
    }

    if (m_eglShadowFbo == 0) {
        gl->glGenFramebuffers(1, &m_eglShadowFbo);
        gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_eglShadowFbo);
        gl->glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D, m_eglShadowTex, 0);
    } else {
        /* Re-attach in case the shadow tex's storage was orphaned by a
         * resize above; safe even when no resize happened. */
        gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_eglShadowFbo);
        gl->glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D, m_eglShadowTex, 0);
    }
    if (m_eglReadFbo == 0) {
        gl->glGenFramebuffers(1, &m_eglReadFbo);
    }
    gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, m_eglReadFbo);
    gl->glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, m_glTextures[slot], 0);

    /* Both FBOs are RGBA8 of identical size — NEAREST is fine and the
     * fastest path on every driver. */
    gl->glBlitFramebuffer(0, 0, w, h, 0, 0, w, h,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);

    /* Detach the imported texture so Qt RHI's later reaping of it
     * (when the lib's pending pool drains) doesn't trip an FBO-
     * incompleteness check on the read FBO. */
    gl->glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, 0, 0);

    gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prevDraw);
    gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, prevRead);

    m_eglShadowHasContent = true;
    return true;
}

void WaywallenDisplay::destroyEglShadow() {
    auto *ctx = QOpenGLContext::currentContext();
    if (ctx) {
        auto *gl = ctx->extraFunctions();
        if (gl) {
            if (m_eglShadowFbo) gl->glDeleteFramebuffers(1, &m_eglShadowFbo);
            if (m_eglReadFbo)   gl->glDeleteFramebuffers(1, &m_eglReadFbo);
            if (m_eglShadowTex) gl->glDeleteTextures(1, &m_eglShadowTex);
        }
    }
    /* Even if no current context (sceneGraph already torn down), zero
     * the names — better to leak GL objects than to call into a dead
     * context. Qt destroys its driver state shortly after this anyway. */
    m_eglShadowFbo = 0;
    m_eglReadFbo   = 0;
    m_eglShadowTex = 0;
    m_eglShadowW = 0;
    m_eglShadowH = 0;
    m_eglShadowHasContent = false;
}

// ---------------------------------------------------------------------------
// Scene graph
// ---------------------------------------------------------------------------

QSGNode *WaywallenDisplay::updatePaintNode(QSGNode *oldNode,
                                           UpdatePaintNodeData *) {
    // Run library-deferred pool destructions on the render thread,
    // where (a) Qt's GL context is current for glDeleteTextures (EGL
    // path) and (b) we can guarantee no in-flight vkQueueSubmit on
    // the blitter is still referencing the released VkImages (Vulkan
    // path). The library itself never calls vkDeviceWaitIdle from the
    // I/O thread anymore — that's what was racing with Qt's RHI on
    // radv and surfacing as VK_ERROR_DEVICE_LOST during rapid
    // wallpaper switches.
    if (m_display) {
#ifdef WW_HAVE_VULKAN
        // Vulkan: skip drain when the blitter's fence is in-flight —
        // its cmd buffer may still be reading the most recently
        // released pool's VkImage (only happens after a post-submit
        // timeout; in steady state fence is cleared between blits).
        // Next iteration's pre-submit wait will clear it and we
        // drain then.
        const bool blitterBusy =
            m_vkBlitterInited && m_vkBlitter.fence_armed;
        if (!blitterBusy) {
            (void)waywallen_display_drain(m_display);
        }
#else
        (void)waywallen_display_drain(m_display);
#endif
    }

#ifdef WW_HAVE_VULKAN
    // Sweep the blitter's deferred-destroy queue. Old shadows
    // queued by ensure_shadow on size change get vkDestroyImage'd
    // here once their per-entry frame countdown elapses — by then
    // Qt RHI has cycled at least one frame boundary and reaped its
    // dependent VkImageView. (Inline destroy in ensure_shadow
    // would race with Qt's release queue and trip
    // VUID-vkDestroyImage-image-01000.)
    if (m_vkBlitterInited) {
        ww_vk_blitter_tick_pending_destroys(&m_vkBlitter);
    }
#endif

    // EGL path: create GL textures lazily on the render thread, then
    // blit the current frame into a host-owned shadow texture. The
    // shadow is what Qt samples — same continuity invariant the Vulkan
    // path provides via ww_vk_blitter_shadow: on `unbind` the lib's
    // imported textures get queued for destruction, but the shadow
    // stays untouched, so the prior frame remains on screen until the
    // next pool's first frame arrives.
    if (m_activeBackend == BackendEGL && m_eglImagesValid
        && !m_glTexturesCreated) {
        ensureGlTextures();
    }
    if (m_activeBackend == BackendEGL && m_glTexturesCreated
        && m_currentSlot >= 0 && m_currentSlot < m_glTextures.size()
        && m_texWidth > 0 && m_texHeight > 0) {
        (void)blitEglShadow(m_currentSlot);
    }

#ifdef WW_HAVE_VULKAN
    // Vulkan path: lazy-init blitter and blit any pending frame into
    // the shadow image. The shadow is what Qt actually samples.
    if (m_activeBackend == BackendVulkan && m_vkImagesValid
        && m_textureCount > 0 && m_texWidth > 0 && m_texHeight > 0) {
        if (!m_vkBlitterInited) {
            int rc = ww_vk_blitter_init(
                &m_vkBlitter,
                reinterpret_cast<VkInstance>(m_vkInstance),
                reinterpret_cast<VkPhysicalDevice>(m_vkPhys),
                reinterpret_cast<VkDevice>(m_vkDevice),
                m_vkQfi,
                reinterpret_cast<VkQueue>(m_vkQueue),
                reinterpret_cast<ww_vk_get_instance_proc_addr_fn>(m_vkGipa));
            if (rc != 0) {
                qCWarning(lcWD, "vk blitter init failed (%d); Vulkan path disabled this session", rc);
                delete oldNode;
                return nullptr;
            }
            m_vkBlitterInited = true;
        }

        // VkFormat must match the imported image's format. Mirror the
        // 8-entry RGBA whitelist in src/drm_fourcc_internal.h /
        // backend_vulkan.c — daemon negotiator picks across this exact
        // set, so any fourcc the producer can emit must map cleanly here.
        VkFormat shadowFmt = VK_FORMAT_UNDEFINED;
        switch (m_vkFourcc) {
        // R8G8B8A8 channel order (memory layout R,G,B,A).
        case 0x34324241: /* AB24 — DRM_FORMAT_ABGR8888 */
        case 0x34324258: /* XB24 — DRM_FORMAT_XBGR8888 */
        case 0x41424752: /* RGBA — DRM_FORMAT_RGBA8888 */
        case 0x58424752: /* RGBX — DRM_FORMAT_RGBX8888 */
            shadowFmt = VK_FORMAT_R8G8B8A8_UNORM; break;
        // B8G8R8A8 channel order (memory layout B,G,R,A).
        case 0x34325241: /* AR24 — DRM_FORMAT_ARGB8888 */
        case 0x34325258: /* XR24 — DRM_FORMAT_XRGB8888 */
        case 0x41524742: /* BGRA — DRM_FORMAT_BGRA8888 */
        case 0x58524742: /* BGRX — DRM_FORMAT_BGRX8888 */
            shadowFmt = VK_FORMAT_B8G8R8A8_UNORM; break;
        default:
            qCWarning(lcWD, "vk blitter: unsupported fourcc 0x%08x; skipping frame",
                      m_vkFourcc);
            delete oldNode;
            return nullptr;
        }
        // If the shadow size/format is about to change, the prior
        // QSGTexture (set on oldNode) holds a VkImageView referencing
        // the old shadow VkImage. ww_vk_blitter_ensure_shadow's
        // vkDestroyImage would then trip
        // VUID-vkDestroyImage-image-01000. Drop the entire node so Qt
        // tears the QSGVulkanTexture (and its view) down cleanly via
        // ownsTexture=true; the rest of updatePaintNode will build a
        // fresh node. (Calling setTexture(nullptr) on oldNode in place
        // crashes in QSGSimpleTextureNode::setTexture on some Qt
        // configurations — the safe path is whole-node replacement.)
        const uint32_t newW = static_cast<uint32_t>(m_texWidth);
        const uint32_t newH = static_cast<uint32_t>(m_texHeight);
        if (oldNode
            && (m_vkBlitter.shadow_image == VK_NULL_HANDLE
                || m_vkBlitter.shadow_w != newW
                || m_vkBlitter.shadow_h != newH
                || m_vkBlitter.shadow_fmt != shadowFmt)) {
            delete oldNode;
            oldNode = nullptr;
        }
        if (ww_vk_blitter_ensure_shadow(&m_vkBlitter, newW, newH, shadowFmt) != 0) {
            delete oldNode;
            return nullptr;
        }

        PendingVkFrame frame;
        {
            QMutexLocker lk(&m_pendingMutex);
            frame = m_pendingVk;
            m_pendingVk = PendingVkFrame{};
        }
        if (frame.valid && frame.slot >= 0
            && frame.slot < m_vkImages.size()) {
            auto imported = reinterpret_cast<VkImage>(m_vkImages[frame.slot]);
            auto acquireSem = reinterpret_cast<VkSemaphore>(frame.acquireSem);
            // blit consumes ownership of releaseSyncobjFd unconditionally.
            ww_vk_blitter_blit(&m_vkBlitter, imported, newW, newH,
                               acquireSem, frame.releaseSyncobjFd);
        }
    }
#endif

    const bool hasTexture =
        // EGL gate: only expose the shadow once at least one frame has
        // been blitted into it, otherwise we'd sample uninitialized
        // GPU memory. After the first frame the shadow stays valid
        // across pool transitions, which is what gives the EGL path
        // the same "keep last frame on switch" continuity the Vulkan
        // path has.
        (m_activeBackend == BackendEGL && m_eglShadowTex != 0
         && m_eglShadowHasContent)
#ifdef WW_HAVE_VULKAN
        // Gate Vulkan-side sampling on actually having a blitted shadow.
        // A bare ensure_shadow leaves the image in VK_IMAGE_LAYOUT_UNDEFINED
        // until the first blit transitions it; sampling an UNDEFINED image
        // trips VUID-vkCmdDraw-None-09600 and on NVIDIA cascades into
        // VK_ERROR_DEVICE_LOST. This happens whenever textures_ready
        // arrives before frame_ready and Qt schedules a paint between
        // the two — common during rapid wallpaper switches.
        || (m_activeBackend == BackendVulkan && m_vkBlitterInited
            && ww_vk_blitter_shadow(&m_vkBlitter) != VK_NULL_HANDLE
            && ww_vk_blitter_shadow_has_content(&m_vkBlitter))
#endif
        ;

    if (!hasTexture || !window()) {
        delete oldNode;
        return nullptr;
    }

    auto *node = static_cast<QSGSimpleTextureNode *>(oldNode);
    if (!node) {
        node = new QSGSimpleTextureNode();
        node->setFiltering(QSGTexture::Linear);
        node->setOwnsTexture(true);
    }

    QSize texSize(m_eglShadowW > 0 ? m_eglShadowW : m_texWidth,
                  m_eglShadowH > 0 ? m_eglShadowH : m_texHeight);
    if (m_activeBackend == BackendVulkan) {
        texSize = QSize(m_texWidth, m_texHeight);
    }
    QSGTexture *sgTex = nullptr;

    if (m_activeBackend == BackendEGL) {
        sgTex = QNativeInterface::QSGOpenGLTexture::fromNative(
            m_eglShadowTex, window(), texSize,
            QQuickWindow::TextureHasAlphaChannel);
    } else if (m_activeBackend == BackendVulkan) {
#ifdef WW_HAVE_VULKAN
        sgTex = QNativeInterface::QSGVulkanTexture::fromNative(
            ww_vk_blitter_shadow(&m_vkBlitter),
            ww_vk_blitter_shadow_layout(&m_vkBlitter),
            window(), texSize,
            QQuickWindow::TextureHasAlphaChannel);
#endif
    }

    if (sgTex) {
        node->setTexture(sgTex);
    } else {
        delete node;
        return nullptr;
    }

    // m_sourceRect / m_destRect arrive in c_on_config and live in
    // texture-pixel and display-pixel space respectively. The item's
    // boundingRect is in QML logical pixels, so convert dest by the
    // ratio (boundingRect / m_displayWidth) which is 1/DPR for the
    // common case. Default-constructed (zero-size) rects mean we
    // haven't seen a SetConfig yet — fall back to identity.
    if (m_sourceRect.width() > 0 && m_sourceRect.height() > 0) {
        node->setSourceRect(m_sourceRect);
    } else {
        node->setSourceRect(QRectF(0, 0, m_texWidth, m_texHeight));
    }

    const QRectF bounds = boundingRect();
    if (m_destRect.width() > 0 && m_destRect.height() > 0
        && m_displayWidth > 0 && m_displayHeight > 0) {
        const qreal sx = bounds.width()  / qreal(m_displayWidth);
        const qreal sy = bounds.height() / qreal(m_displayHeight);
        node->setRect(QRectF(m_destRect.x()      * sx,
                             m_destRect.y()      * sy,
                             m_destRect.width()  * sx,
                             m_destRect.height() * sy));
    } else {
        node->setRect(bounds);
    }
    return node;
}
