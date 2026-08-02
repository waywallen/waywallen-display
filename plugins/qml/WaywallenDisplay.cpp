#include "WaywallenDisplay.hpp"

#include <waywallen_display.h>

#include <QCryptographicHash>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDebug>
#include <QLoggingCategory>
#include <QMatrix4x4>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QQuickGraphicsConfiguration>
#include <QQuickWindow>
#include <QRunnable>
#include <QScreen>
#include <QSGImageNode>
#include <QSGRectangleNode>
#include <QSGRendererInterface>
#include <QSGTransformNode>
#include <QWheelEvent>
#include <QtGui/qopenglcontext_platform.h>
#include <QtQuick/qsgtexture_platform.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <cerrno>
#include <limits>
#include <utility>
#include <unistd.h>

Q_LOGGING_CATEGORY(lcWD, "waywallen.display")

class RenderSessionResources {
public:
    ~RenderSessionResources() {
        if (display) {
            qCCritical(lcWD,
                       "render-session cleanup job was discarded; GPU resources are retained "
                       "until process exit");
        }
    }

    void shutdown() {
#ifdef WW_HAVE_VULKAN
        if (vkBlitterInited) {
            ww_vk_blitter_shutdown(&vkBlitter);
            vkBlitterInited = false;
        }
#endif
        if (display) {
            waywallen_display_shutdown(display);
            display = nullptr;
        }
        destroyEglShadow();
        eglDisplay = nullptr;
    }

    void destroyEglShadow() {
        auto* ctx = QOpenGLContext::currentContext();
        if (ctx) {
            auto* gl = ctx->extraFunctions();
            if (gl) {
                if (eglShadowFbo) gl->glDeleteFramebuffers(1, &eglShadowFbo);
                if (eglCandidateFbo) gl->glDeleteFramebuffers(1, &eglCandidateFbo);
                if (eglReadFbo) gl->glDeleteFramebuffers(1, &eglReadFbo);
                if (eglShadowTex) gl->glDeleteTextures(1, &eglShadowTex);
                if (eglCandidateTex) gl->glDeleteTextures(1, &eglCandidateTex);
            }
        }
        eglShadowFbo           = 0;
        eglReadFbo             = 0;
        eglShadowTex           = 0;
        eglCandidateFbo        = 0;
        eglCandidateTex        = 0;
        eglCandidateW          = 0;
        eglCandidateH          = 0;
        eglShadowW             = 0;
        eglShadowH             = 0;
        eglShadowHasContent    = false;
        eglCandidateHasContent = false;
    }

    bool discardEglCandidate() {
        if (! eglCandidateTex && ! eglCandidateFbo) return true;
        auto* ctx = QOpenGLContext::currentContext();
        auto* gl  = ctx ? ctx->extraFunctions() : nullptr;
        if (! gl) return false;
        if (eglCandidateFbo) gl->glDeleteFramebuffers(1, &eglCandidateFbo);
        if (eglCandidateTex) gl->glDeleteTextures(1, &eglCandidateTex);
        eglCandidateFbo        = 0;
        eglCandidateTex        = 0;
        eglCandidateW          = 0;
        eglCandidateH          = 0;
        eglCandidateHasContent = false;
        return true;
    }

    bool commitEglCandidate() {
        if (! eglCandidateTex || ! eglCandidateFbo || ! eglCandidateHasContent) return false;
        auto* ctx = QOpenGLContext::currentContext();
        auto* gl  = ctx ? ctx->extraFunctions() : nullptr;
        if ((eglShadowTex || eglShadowFbo) && ! gl) return false;

        const uint oldFbo      = eglShadowFbo;
        const uint oldTex      = eglShadowTex;
        eglShadowFbo           = eglCandidateFbo;
        eglShadowTex           = eglCandidateTex;
        eglShadowW             = eglCandidateW;
        eglShadowH             = eglCandidateH;
        eglShadowHasContent    = true;
        eglCandidateFbo        = 0;
        eglCandidateTex        = 0;
        eglCandidateW          = 0;
        eglCandidateH          = 0;
        eglCandidateHasContent = false;

        if (oldFbo) gl->glDeleteFramebuffers(1, &oldFbo);
        if (oldTex) gl->glDeleteTextures(1, &oldTex);
        return true;
    }

    waywallen_display_t* display { nullptr };
    uint                 eglShadowTex { 0 };
    uint                 eglShadowFbo { 0 };
    uint                 eglReadFbo { 0 };
    int                  eglShadowW { 0 };
    int                  eglShadowH { 0 };
    bool                 eglShadowHasContent { false };
    uint                 eglCandidateTex { 0 };
    uint                 eglCandidateFbo { 0 };
    int                  eglCandidateW { 0 };
    int                  eglCandidateH { 0 };
    bool                 eglCandidateHasContent { false };
    void*                eglDisplay { nullptr };
#ifdef WW_HAVE_VULKAN
    ww_vk_blitter_t vkBlitter {};
    bool            vkBlitterInited { false };
#endif
};

namespace
{
/* Tiny QRunnable adapter so cleanup() can post a render-thread shutdown
 * without keeping the QML item alive for the duration. The lib's
 * shutdown is bounded (close + 4 drain iterations + free), so no
 * watchdog needed here. */
class FnRunnable : public QRunnable {
public:
    explicit FnRunnable(std::function<void()> fn): m_fn(std::move(fn)) { setAutoDelete(true); }
    void run() override {
        if (m_fn) m_fn();
    }

private:
    std::function<void()> m_fn;
};

class RenderSessionCleanupJob : public QRunnable {
public:
    explicit RenderSessionCleanupJob(std::shared_ptr<RenderSessionResources> resources)
        : m_resources(std::move(resources)) {
        setAutoDelete(true);
    }

    void run() override {
        m_resources->shutdown();
        m_resources.reset();
    }

private:
    std::shared_ptr<RenderSessionResources> m_resources;
};

class PresentationNode final : public QSGNode {
public:
    enum class TextureBackend
    {
        None,
        OpenGL,
        Vulkan,
    };

    static PresentationNode* create(QQuickWindow* window) {
        auto* clear = window->createRectangleNode();
        auto* image = window->createImageNode();
        if (! clear || ! image) {
            delete clear;
            delete image;
            return nullptr;
        }
        return new PresentationNode(clear, image);
    }

    QSGRectangleNode* clearNode;
    QSGTransformNode* transformNode;
    QSGImageNode*     imageNode;
    TextureBackend    textureBackend { TextureBackend::None };
    quintptr          nativeTexture { 0 };
    QSize             textureSize;

    void installTexture(QSGTexture* texture, TextureBackend backend, quintptr handle,
                        const QSize& size) {
        imageNode->setTexture(texture);
        textureBackend = backend;
        nativeTexture  = handle;
        textureSize    = size;
    }

    bool wraps(TextureBackend backend, quintptr handle, const QSize& size) const {
        return textureBackend == backend && nativeTexture == handle && textureSize == size;
    }

private:
    PresentationNode(QSGRectangleNode* clear, QSGImageNode* image)
        : clearNode(clear), transformNode(new QSGTransformNode()), imageNode(image) {
        imageNode->setFiltering(QSGTexture::Linear);
        imageNode->setOwnsTexture(true);
        appendChildNode(clearNode);
        appendChildNode(transformNode);
        transformNode->appendChildNode(imageNode);
    }
};

QString screenPart(const QString& value) { return value.trimmed(); }
} // namespace

// Linux input event codes — matches wlroots / Wayland convention so
// renderer plugins consuming forwarded events get a familiar enum.
static constexpr uint32_t WW_BTN_LEFT   = 0x110;
static constexpr uint32_t WW_BTN_RIGHT  = 0x111;
static constexpr uint32_t WW_BTN_MIDDLE = 0x112;
static constexpr uint32_t WW_BTN_SIDE   = 0x113;
static constexpr uint32_t WW_BTN_EXTRA  = 0x114;

static uint32_t qtButtonToLinuxCode(Qt::MouseButton b) {
    switch (b) {
    case Qt::LeftButton: return WW_BTN_LEFT;
    case Qt::RightButton: return WW_BTN_RIGHT;
    case Qt::MiddleButton: return WW_BTN_MIDDLE;
    case Qt::BackButton: return WW_BTN_SIDE;
    case Qt::ForwardButton: return WW_BTN_EXTRA;
    default: return 0;
    }
}

static uint32_t qtModifiers(Qt::KeyboardModifiers modifiers) {
    uint32_t flags = 0;
    if (modifiers.testFlag(Qt::ShiftModifier)) flags |= WAYWALLEN_POINTER_MOD_SHIFT;
    if (modifiers.testFlag(Qt::ControlModifier)) flags |= WAYWALLEN_POINTER_MOD_CTRL;
    if (modifiers.testFlag(Qt::AltModifier)) flags |= WAYWALLEN_POINTER_MOD_ALT;
    if (modifiers.testFlag(Qt::MetaModifier)) flags |= WAYWALLEN_POINTER_MOD_SUPER;
    return flags;
}

// ---------------------------------------------------------------------------
// C library log → Qt log category bridge
// ---------------------------------------------------------------------------

static void qtLogBridge(waywallen_log_level_t level, const char* msg, void*) {
    switch (level) {
    case WAYWALLEN_LOG_DEBUG: qCDebug(lcWD, "%s", msg); break;
    case WAYWALLEN_LOG_INFO: qCInfo(lcWD, "%s", msg); break;
    case WAYWALLEN_LOG_WARN: qCWarning(lcWD, "%s", msg); break;
    case WAYWALLEN_LOG_ERROR: qCCritical(lcWD, "%s", msg); break;
    }
}

// ---------------------------------------------------------------------------
// C callback trampolines
// ---------------------------------------------------------------------------

static PresentationState::Config
toConfigSnapshot(const waywallen_composition_config_t& composition) {
    PresentationState::Config config;
    config.valid            = true;
    config.bufferGeneration = composition.buffer_generation;
    config.configGeneration = composition.generation;
    config.sourceRect       = QRectF(static_cast<qreal>(composition.source_rect.x),
                                     static_cast<qreal>(composition.source_rect.y),
                                     static_cast<qreal>(composition.source_rect.w),
                                     static_cast<qreal>(composition.source_rect.h));
    config.destRect         = QRectF(static_cast<qreal>(composition.dest_rect.x),
                                     static_cast<qreal>(composition.dest_rect.y),
                                     static_cast<qreal>(composition.dest_rect.w),
                                     static_cast<qreal>(composition.dest_rect.h));
    config.clearColor       = QColor::fromRgbF(static_cast<qreal>(composition.clear_color.r),
                                               static_cast<qreal>(composition.clear_color.g),
                                               static_cast<qreal>(composition.clear_color.b),
                                               static_cast<qreal>(composition.clear_color.a));
    config.transform        = composition.transform;
    return config;
}

void WaywallenDisplay::c_on_binding_ready(void* ud, const waywallen_binding_t* binding) {
    auto*       self     = static_cast<WaywallenDisplay*>(ud);
    const auto* t        = &binding->textures;
    const auto  config   = toConfigSnapshot(binding->config);
    self->m_textureCount = t->count;
    bool imported        = false;

    if (t->backend == WAYWALLEN_BACKEND_EGL && t->egl_images) {
        qCInfo(lcWD,
               "binding ready: EGL, count=%u, size=%ux%u, fourcc=0x%x",
               t->count,
               t->tex_width,
               t->tex_height,
               t->fourcc);
        self->m_eglImagesValid    = true;
        self->m_glTexturesCreated = false;
        self->m_glTextures.clear();
        imported = true;
    } else if (t->backend == WAYWALLEN_BACKEND_VULKAN && t->vk_images) {
        qCInfo(lcWD,
               "binding ready: Vulkan, count=%u, size=%ux%u, fourcc=0x%x",
               t->count,
               t->tex_width,
               t->tex_height,
               t->fourcc);
        self->m_vkImagesValid = true;
        self->m_vkImages.resize(static_cast<int>(t->count));
        for (uint32_t i = 0; i < t->count; i++)
            self->m_vkImages[static_cast<int>(i)] = t->vk_images[i];
        imported = true;
    } else {
        qCWarning(lcWD, "binding ready: backend=%d but no handles", t->backend);
        self->m_eglImagesValid = false;
        self->m_vkImagesValid  = false;
    }
    {
        QMutexLocker lk(&self->m_pendingMutex);
        self->m_presentationState.beginIncoming(t->buffer_generation,
                                                static_cast<int>(t->tex_width),
                                                static_cast<int>(t->tex_height),
                                                t->fourcc,
                                                imported);
        const auto result = self->m_presentationState.applyConfig(config);
        if (imported && result == PresentationState::ConfigResult::Rejected) {
            qCCritical(lcWD,
                       "atomic binding rejected composition=%llu buffer=%llu",
                       qulonglong(binding->config.generation),
                       qulonglong(binding->config.buffer_generation));
            return;
        }
    }
    self->setStreamState(Active);
}

void WaywallenDisplay::c_on_textures_releasing(void* ud, const waywallen_textures_t* t) {
    auto* self = static_cast<WaywallenDisplay*>(ud);
    qCInfo(lcWD, "textures releasing: generation=%llu", qulonglong(t->buffer_generation));
    self->flushPendingRelease();

    // GL textures are owned by the C library (created via
    // waywallen_display_create_gl_texture); the library's cleanup
    // will delete them together with the EGLImages.
    self->m_eglImagesValid    = false;
    self->m_glTexturesCreated = false;
    self->m_glTextures.clear();
    {
        QMutexLocker lk(&self->m_pendingMutex);
        self->m_presentationState.retireIncoming(t->buffer_generation);
        if (self->m_preparedEglContent.bufferGeneration == t->buffer_generation) {
            self->m_preparedEglContent = ContentSnapshot {};
        }
    }
    self->m_vkImagesValid = false;
    self->m_vkImages.clear();
    self->m_textureCount = 0;

#ifdef WW_HAVE_VULKAN
    {
        QMutexLocker lk(&self->m_pendingMutex);
        if (self->m_pendingVk.valid && self->m_pendingVk.releaseSyncobjFd >= 0) {
            // The queued frame was never submitted to GPU work.
            self->signalFrameRelease(self->m_pendingVk.releaseSyncobjFd,
                                     self->m_pendingVk.bufferGeneration,
                                     self->m_pendingVk.seq,
                                     "retired Vulkan frame");
        }
        self->m_pendingVk = PendingVkFrame {};
    }
    // Blitter teardown happens on the render thread (sceneGraphInvalidated
    // or cleanup()) — it owns Vulkan handles bound to a specific device.
#endif

    self->setStreamState(Inactive);
    self->update();
}

void WaywallenDisplay::c_on_composition_config(void*                                 ud,
                                               const waywallen_composition_config_t* composition) {
    auto*      self   = static_cast<WaywallenDisplay*>(ud);
    const auto config = toConfigSnapshot(*composition);

    bool updatesPresented = false;
    {
        QMutexLocker lk(&self->m_pendingMutex);
        const auto   result = self->m_presentationState.applyConfig(config);
        if (result == PresentationState::ConfigResult::Rejected) {
            qCWarning(lcWD,
                      "composition generation=%llu targets unexpected buffer generation=%llu",
                      qulonglong(composition->generation),
                      qulonglong(composition->buffer_generation));
            return;
        }
        updatesPresented = result == PresentationState::ConfigResult::PresentedUpdated;
    }
    if (updatesPresented) {
        self->setPresentedClearColor(config.clearColor);
        self->update();
    }
}

void WaywallenDisplay::c_on_frame_ready(void* ud, const waywallen_frame_t* f) {
    auto* self = static_cast<WaywallenDisplay*>(ud);
    self->m_framesReceived++;

    bool validIncoming = false;
    {
        QMutexLocker    lk(&self->m_pendingMutex);
        ContentSnapshot incoming;
        validIncoming = self->m_presentationState.incomingFor(f->buffer_generation, incoming);
    }
    if (! validIncoming) {
        qCWarning(lcWD,
                  "dropping frame for unconfigured generation=%llu",
                  qulonglong(f->buffer_generation));
        if (f->release_syncobj_fd >= 0) {
            self->signalFrameRelease(
                f->release_syncobj_fd, f->buffer_generation, f->seq, "unconfigured frame");
        }
        emit self->framesReceivedChanged();
        return;
    }
#ifdef WW_HAVE_VULKAN
    if (self->m_activeBackend == BackendVulkan) {
        // Hand-off to render thread. If a prior frame is still queued
        // (render thread didn't blit it yet), drop it: signal its
        // release_syncobj immediately. The buffer was never read.
        QMutexLocker lk(&self->m_pendingMutex);
        if (self->m_pendingVk.valid && self->m_pendingVk.releaseSyncobjFd >= 0) {
            self->signalFrameRelease(self->m_pendingVk.releaseSyncobjFd,
                                     self->m_pendingVk.bufferGeneration,
                                     self->m_pendingVk.seq,
                                     "superseded Vulkan frame");
        }
        self->m_pendingVk.valid            = true;
        self->m_pendingVk.slot             = static_cast<int>(f->buffer_index);
        self->m_pendingVk.acquireSem       = f->vk_acquire_semaphore;
        self->m_pendingVk.releaseSyncobjFd = f->release_syncobj_fd;
        self->m_pendingVk.bufferGeneration = f->buffer_generation;
        self->m_pendingVk.seq              = f->seq;
    } else
#endif
        if (self->m_activeBackend == BackendEGL) {
        // Arrival-driven blit: queue the slot and dispatch a render-thread
        // job that does the GPU copy. BeforeSynchronizingStage runs on
        // the render thread immediately before sync (so before our
        // updatePaintNode), guaranteeing the shadow is fresh by the time
        // Qt samples it. NoStage would race with sync. The queued slot
        // is copied exactly once by the job that consumes it.
        {
            QMutexLocker lk(&self->m_pendingMutex);
            if (self->m_pendingEgl.valid && self->m_pendingEgl.releaseSyncobjFd >= 0) {
                self->signalFrameRelease(self->m_pendingEgl.releaseSyncobjFd,
                                         self->m_pendingEgl.bufferGeneration,
                                         self->m_pendingEgl.seq,
                                         "superseded EGL frame");
            }
            self->m_pendingEgl.valid            = true;
            self->m_pendingEgl.slot             = static_cast<int>(f->buffer_index);
            self->m_pendingEgl.releaseSyncobjFd = f->release_syncobj_fd;
            self->m_pendingEgl.bufferGeneration = f->buffer_generation;
            self->m_pendingEgl.seq              = f->seq;
        }
        if (auto* w = self->window()) {
            QPointer<WaywallenDisplay> guard(self);
            w->scheduleRenderJob(new FnRunnable([guard]() {
                                     if (guard) guard->renderThreadBlitEgl();
                                 }),
                                 QQuickWindow::BeforeSynchronizingStage);
        }
    } else if (f->release_syncobj_fd >= 0) {
        // No active backend yet, so no ownership was transferred to GPU work.
        self->signalFrameRelease(
            f->release_syncobj_fd, f->buffer_generation, f->seq, "frame without active backend");
    }

    emit self->framesReceivedChanged();
    self->update();
}

void WaywallenDisplay::c_on_presentation_snapshot(
    void* ud, const waywallen_presentation_snapshot_t* presentation) {
    auto* self = static_cast<WaywallenDisplay*>(ud);
    self->applyPresentationSnapshot(*presentation);
}

void WaywallenDisplay::c_on_presentation_state(void*                                 ud,
                                               const waywallen_presentation_state_t* state) {
    auto* self = static_cast<WaywallenDisplay*>(ud);
    self->applyPresentationState(*state);
}

void WaywallenDisplay::c_on_disconnected(void* ud, int err, const char* msg) {
    auto* self    = static_cast<WaywallenDisplay*>(ud);
    auto* display = self->displayHandle();
    if (! display) return;
    const auto reason =
        static_cast<DisconnectReason>(waywallen_display_last_disconnect_reason(display));
    const QString message = QString::fromUtf8(waywallen_display_last_disconnect_message(display));
    if (self->m_lastReason != reason || self->m_lastMessage != message) {
        self->m_lastReason  = reason;
        self->m_lastMessage = message;
        emit self->lastDisconnectChanged();
    }
    self->handleDisconnect(err, msg);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

WaywallenDisplay::WaywallenDisplay(QQuickItem* parent): QQuickItem(parent) {
    setFlag(ItemHasContents, true);
    waywallen_display_set_log_callback(qtLogBridge, nullptr);

    m_updateSizeTimer.setSingleShot(true);
    m_updateSizeTimer.setInterval(100);
    connect(&m_updateSizeTimer, &QTimer::timeout, this, &WaywallenDisplay::pushSizeUpdate);
}

WaywallenDisplay::~WaywallenDisplay() { cleanup(); }

void WaywallenDisplay::releaseResources() {
    cleanup();
    setConnState(Disconnected);
    setStreamState(Inactive);
    QQuickItem::releaseResources();
}

waywallen_display_t* WaywallenDisplay::displayHandle() const {
    auto resources = renderSessionResources();
    return resources ? resources->display : nullptr;
}

std::shared_ptr<RenderSessionResources> WaywallenDisplay::renderSessionResources() const {
    QMutexLocker lock(&m_resourcesMutex);
    return m_renderResources;
}

std::shared_ptr<RenderSessionResources> WaywallenDisplay::takeRenderSessionResources() {
    std::shared_ptr<RenderSessionResources> resources;
    {
        QMutexLocker lock(&m_resourcesMutex);
        resources = std::move(m_renderResources);
    }
    if (resources && resources->display) waywallen_display_close(resources->display);
    return resources;
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

    flushPendingRelease();
#ifdef WW_HAVE_VULKAN
    {
        QMutexLocker lk(&m_pendingMutex);
        if (m_pendingVk.valid && m_pendingVk.releaseSyncobjFd >= 0) {
            signalFrameRelease(m_pendingVk.releaseSyncobjFd,
                               m_pendingVk.bufferGeneration,
                               m_pendingVk.seq,
                               "cleanup Vulkan frame");
        }
        m_pendingVk = PendingVkFrame {};
    }
#endif

    if (auto resources = takeRenderSessionResources()) {
        QQuickWindow* w = window();
        if (w && w->isSceneGraphInitialized()) {
            /* updateDirtyNodes() detaches the old owned QSGTexture during
             * sync; native shadows can only be destroyed after that. */
            w->scheduleRenderJob(new RenderSessionCleanupJob(std::move(resources)),
                                 QQuickWindow::AfterSynchronizingStage);
            w->update();
        } else if (m_activeBackend == BackendNone) {
            resources->shutdown();
        } else {
            qCCritical(lcWD,
                       "no render context for shutdown; GPU resources are retained until "
                       "process exit");
            (void)new std::shared_ptr<RenderSessionResources>(std::move(resources));
        }
    }

    m_updateSizeTimer.stop();
    m_lastPushedWidth  = -1;
    m_lastPushedHeight = -1;

    m_eglImagesValid    = false;
    m_glTexturesCreated = false;
    m_glTextures.clear();
    m_vkImagesValid = false;
    m_vkImages.clear();
    {
        QMutexLocker lk(&m_pendingMutex);
        m_presentationState.reset();
        m_preparedEglContent         = ContentSnapshot {};
        m_notifiedPresentationSerial = m_presentationSerial;
    }
    m_textureCount  = 0;
    m_activeBackend = BackendNone;
    resetPresentation();

    if (m_displayId != 0) {
        m_displayId = 0;
        emit displayIdChanged();
    }
}

void WaywallenDisplay::setPresentedClearColor(const QColor& color) {
    if (m_clearColor == color) return;
    m_clearColor = color;
    emit clearColorChanged();
}

void WaywallenDisplay::publishPresentationCommit(qulonglong serial, const QColor& clearColor) {
    {
        QMutexLocker lk(&m_pendingMutex);
        if (serial <= m_notifiedPresentationSerial) return;
        m_notifiedPresentationSerial = serial;
    }
    setPresentedClearColor(clearColor);
    m_contentRevision++;
    emit contentRevisionChanged();
}

void WaywallenDisplay::applyPresentationSnapshot(
    const waywallen_presentation_snapshot_t& presentation) {
    const auto kind = static_cast<PauseEffectKind>(presentation.config.pause_effect.kind);
    const bool changed =
        m_pauseEffectKind != kind ||
        m_blurRadius != static_cast<int>(presentation.config.pause_effect.blur.radius) ||
        m_pauseEffectActive != presentation.state.pause_effect.active ||
        m_presentationConfigGeneration != presentation.config.generation ||
        m_presentationStateGeneration != presentation.state.generation;
    m_pauseEffectKind              = kind;
    m_blurRadius                   = static_cast<int>(presentation.config.pause_effect.blur.radius);
    m_pauseEffectActive            = presentation.state.pause_effect.active;
    m_presentationConfigGeneration = presentation.config.generation;
    m_presentationStateGeneration  = presentation.state.generation;
    qCInfo(lcWD,
           "presentation snapshot: config=%llu state=%llu pause-effect kind=%d active=%d "
           "radius=%d",
           m_presentationConfigGeneration,
           m_presentationStateGeneration,
           int(m_pauseEffectKind),
           m_pauseEffectActive,
           m_blurRadius);
    if (changed) emit presentationChanged();
}

void WaywallenDisplay::applyPresentationState(const waywallen_presentation_state_t& state) {
    const bool changed            = m_pauseEffectActive != state.pause_effect.active ||
                                    m_presentationStateGeneration != state.generation;
    m_pauseEffectActive           = state.pause_effect.active;
    m_presentationStateGeneration = state.generation;
    qCDebug(lcWD,
            "presentation state: config=%llu state=%llu pause-effect active=%d",
            qulonglong(state.config_generation),
            qulonglong(state.generation),
            state.pause_effect.active);
    if (changed) emit presentationChanged();
}

void WaywallenDisplay::resetPresentation() {
    const bool changed             = m_pauseEffectKind != NonePauseEffect || m_pauseEffectActive ||
                                     m_blurRadius != 30 || m_presentationConfigGeneration != 0 ||
                                     m_presentationStateGeneration != 0;
    m_pauseEffectKind              = NonePauseEffect;
    m_blurRadius                   = 30;
    m_pauseEffectActive            = false;
    m_presentationConfigGeneration = 0;
    m_presentationStateGeneration  = 0;
    if (changed) emit presentationChanged();
}

void WaywallenDisplay::commitPresentedContent(uint64_t generation, int width, int height,
                                              uint32_t fourcc, const ConfigSnapshot& config) {
    PresentationState::CommitResult result = PresentationState::CommitResult::Rejected;
    qulonglong                      serial = 0;
    {
        QMutexLocker    lk(&m_pendingMutex);
        ContentSnapshot content;
        content.valid            = true;
        content.bufferGeneration = generation;
        content.width            = width;
        content.height           = height;
        content.fourcc           = fourcc;
        content.config           = config;
        result                   = m_presentationState.commit(content);
        if (result == PresentationState::CommitResult::SourceChanged) {
            serial = ++m_presentationSerial;
        }
    }
    if (result == PresentationState::CommitResult::Rejected) {
        qCWarning(lcWD, "presentation commit rejected for generation=%llu", qulonglong(generation));
        return;
    }
    if (result == PresentationState::CommitResult::SameSourceUpdated) return;

    QPointer<WaywallenDisplay> guard(this);
    QMetaObject::invokeMethod(
        this,
        [guard, serial, clearColor = config.clearColor]() {
            if (guard) guard->publishPresentationCommit(serial, clearColor);
        },
        Qt::QueuedConnection);
}

// ---------------------------------------------------------------------------
// Properties
// ---------------------------------------------------------------------------

QString WaywallenDisplay::screenIdentityKey() const {
    auto* w = window();
    auto* s = w ? w->screen() : nullptr;
    if (! s) return {};

    return QStringLiteral("name=%1|manufacturer=%2|model=%3|serial=%4")
        .arg(screenPart(s->name()),
             screenPart(s->manufacturer()),
             screenPart(s->model()),
             screenPart(s->serialNumber()));
}

QString WaywallenDisplay::effectiveInstanceId() const {
    if (! m_instanceId.isEmpty()) return m_instanceId;

    const auto key = screenIdentityKey();
    if (key.isEmpty()) return {};

    const auto md5 = QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Md5).toHex();
    return QStringLiteral("kde-") + QString::fromLatin1(md5);
}

uint32_t WaywallenDisplay::screenRefreshMhz() const {
    auto* w = window();
    auto* s = w ? w->screen() : nullptr;
    if (! s || s->refreshRate() <= 0.0) return 60000;

    const auto mhz = qRound64(s->refreshRate() * 1000.0);
    if (mhz <= 0 || mhz > std::numeric_limits<uint32_t>::max()) return 60000;
    return static_cast<uint32_t>(mhz);
}

void WaywallenDisplay::setSocketPath(const QString& path) {
    if (m_socketPath == path) return;
    m_socketPath = path;
    emit socketPathChanged();
}

void WaywallenDisplay::setDisplayName(const QString& name) {
    if (m_displayName == name) return;
    m_displayName = name;
    emit displayNameChanged();
}

void WaywallenDisplay::setInstanceId(const QString& id) {
    if (m_instanceId == id) return;
    m_instanceId = id;
    emit instanceIdChanged();
}

void WaywallenDisplay::setDisplayWidth(int w) {
    if (m_displayWidth == w) return;
    m_displayWidth = w;
    emit displayWidthChanged();
    if (displayHandle()) m_updateSizeTimer.start();
}

void WaywallenDisplay::setDisplayHeight(int h) {
    if (m_displayHeight == h) return;
    m_displayHeight = h;
    emit displayHeightChanged();
    if (displayHandle()) m_updateSizeTimer.start();
}

void WaywallenDisplay::pushSizeUpdate() {
    auto* display = displayHandle();
    if (! display) return;
    if (waywallen_display_conn_state(display) != WAYWALLEN_CONN_CONNECTED) {
        // Drop silently — once the handshake completes, register_display
        // already carried the latest dims via begin_connect.
        return;
    }
    if (m_displayWidth <= 0 || m_displayHeight <= 0) return;
    const uint32_t refreshMhz = screenRefreshMhz();
    if (m_lastPushedWidth == m_displayWidth && m_lastPushedHeight == m_displayHeight &&
        m_lastPushedRefreshMhz == refreshMhz) {
        return;
    }
    const waywallen_display_metrics_t metrics {
        static_cast<uint32_t>(m_displayWidth),
        static_cast<uint32_t>(m_displayHeight),
        refreshMhz,
    };
    int rc = waywallen_display_set_metrics(display, &metrics);
    if (rc != 0) {
        qCWarning(lcWD,
                  "set_metrics(%d, %d, %u) failed: %d",
                  m_displayWidth,
                  m_displayHeight,
                  refreshMhz,
                  rc);
        return;
    }
    m_lastPushedWidth      = m_displayWidth;
    m_lastPushedHeight     = m_displayHeight;
    m_lastPushedRefreshMhz = refreshMhz;
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
        if (enabled && ! m_filterInstalled) {
            window()->installEventFilter(this);
            m_filterInstalled = true;
        } else if (! enabled && m_filterInstalled) {
            window()->removeEventFilter(this);
            m_filterInstalled = false;
        }
    }
    emit mouseForwardEnabledChanged();
}

bool WaywallenDisplay::eventFilter(QObject* obj, QEvent* ev) {
    auto* display = displayHandle();
    if (! m_mouseForwardEnabled || obj != window() || ! display) {
        return false;
    }
    if (waywallen_display_conn_state(display) != WAYWALLEN_CONN_CONNECTED) {
        return false;
    }

    const QRectF bounds = boundingRect();
    if (bounds.width() <= 0 || bounds.height() <= 0) return false;

    auto toSurface = [&](const QPointF& scenePos, float& px, float& py) -> bool {
        const QPointF inItem = mapFromScene(scenePos);
        if (! bounds.contains(inItem)) return false;
        const float sx = float(m_displayWidth) / float(bounds.width());
        const float sy = float(m_displayHeight) / float(bounds.height());
        px             = float(inItem.x()) * sx;
        py             = float(inItem.y()) * sy;
        return true;
    };

    switch (ev->type()) {
    case QEvent::MouseMove: {
        auto* me = static_cast<QMouseEvent*>(ev);
        float px, py;
        if (! toSurface(me->scenePosition(), px, py)) return false;
        const uint64_t ts = uint64_t(me->timestamp()) * 1000ull;
        (void)waywallen_display_send_pointer_motion(
            display, px, py, ts, qtModifiers(me->modifiers()));
        break;
    }
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease: {
        auto* me = static_cast<QMouseEvent*>(ev);
        float px, py;
        if (! toSurface(me->scenePosition(), px, py)) return false;
        const uint32_t code = qtButtonToLinuxCode(me->button());
        if (code == 0) return false;
        const uint64_t ts    = uint64_t(me->timestamp()) * 1000ull;
        const auto     state = (ev->type() == QEvent::MouseButtonPress)
                                   ? WAYWALLEN_POINTER_BUTTON_STATE_PRESSED
                                   : WAYWALLEN_POINTER_BUTTON_STATE_RELEASED;
        (void)waywallen_display_send_pointer_button(
            display, px, py, code, state, ts, qtModifiers(me->modifiers()));
        break;
    }
    case QEvent::Wheel: {
        auto* we = static_cast<QWheelEvent*>(ev);
        float px, py;
        if (! toSurface(we->position(), px, py)) return false;
        const QPoint angle = we->angleDelta();
        const float  dx    = float(angle.x()) / 120.0f;
        const float  dy    = float(angle.y()) / 120.0f;
        if (dx == 0.0f && dy == 0.0f) return false;
        const uint64_t ts = uint64_t(we->timestamp()) * 1000ull;
        (void)waywallen_display_send_pointer_axis(display,
                                                  px,
                                                  py,
                                                  dx,
                                                  dy,
                                                  WAYWALLEN_POINTER_AXIS_SOURCE_WHEEL,
                                                  ts,
                                                  qtModifiers(we->modifiers()));
        break;
    }
    default: break;
    }
    armWriteNotifier();
    return false;
}

void WaywallenDisplay::setConnState(ConnState s) {
    if (m_connState == s) return;
    m_connState = s;
    emit connStateChanged();
    // Re-send any post-handshake state that's only valid against a
    // Connected daemon: window_state (autopause reporter) carries the
    // most recent QML-visible bitmask, which the lib does not
    // remember across reconnects.
    if (s == Connected && displayHandle() && m_windowStateFlagsDirty) {
        if (waywallen_display_set_window_state(displayHandle(), m_windowStateFlags) ==
            WAYWALLEN_OK) {
            m_windowStateFlagsDirty = false;
            armWriteNotifier();
        }
    }
}

void WaywallenDisplay::setWindowStateFlags(quint32 flags) {
    // Always emit the change signal so QML bindings track the
    // property; the daemon push is best-effort and gated on
    // Connected.
    const bool changed = m_windowStateFlags != flags;
    m_windowStateFlags = flags;
    if (displayHandle()) {
        if (waywallen_display_set_window_state(displayHandle(), flags) == WAYWALLEN_OK) {
            m_windowStateFlagsDirty = false;
            armWriteNotifier();
        } else {
            // Lib returned ERR_STATE / NOMEM. Mark dirty so we retry
            // on the next reconnect or external poke; armWriteNotifier
            // would be a no-op anyway with nothing queued.
            m_windowStateFlagsDirty = true;
        }
    } else {
        m_windowStateFlagsDirty = true;
    }
    if (changed) emit windowStateFlagsChanged();
}

void WaywallenDisplay::setPresentationCapabilities(quint32 capabilities) {
    if (m_presentationCapabilities == capabilities) return;
    if (displayHandle()) {
        qCWarning(lcWD, "presentation capabilities apply on the next connection");
    }
    m_presentationCapabilities = capabilities;
    emit presentationCapabilitiesChanged();
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
static QOpenGLContext* s_qtGlCtxForProcAddr = nullptr;
static void*           qtEglGetProcAddress(const char* name) {
    if (! s_qtGlCtxForProcAddr) return nullptr;
    auto fn = s_qtGlCtxForProcAddr->getProcAddress(name);
    return reinterpret_cast<void*>(fn);
}

bool WaywallenDisplay::bindEglBackend() {
    auto resources = renderSessionResources();
    if (! resources) return false;
    auto* rif   = window()->rendererInterface();
    auto* glCtx = static_cast<QOpenGLContext*>(
        rif ? rif->getResource(window(), QSGRendererInterface::OpenGLContextResource) : nullptr);
    if (! glCtx) {
        qCWarning(lcWD, "OpenGL API but no QOpenGLContext");
        return false;
    }

    auto* eglIface = glCtx->nativeInterface<QNativeInterface::QEGLContext>();
    if (! eglIface) {
        qCWarning(lcWD, "OpenGL context has no EGL interface (GLX?)");
        return false;
    }

    s_qtGlCtxForProcAddr = glCtx;

    waywallen_egl_ctx_t egl_ctx {};
    egl_ctx.egl_display      = eglIface->display();
    egl_ctx.get_proc_address = &qtEglGetProcAddress;
    int rc                   = waywallen_display_bind_egl(displayHandle(), &egl_ctx);
    if (rc != WAYWALLEN_OK) {
        qCWarning(lcWD, "bind_egl failed: %d", rc);
        return false;
    }
    resources->eglDisplay = egl_ctx.egl_display;
    qCInfo(lcWD, "bound EGL backend, display=%p", static_cast<void*>(egl_ctx.egl_display));
    return true;
}

bool WaywallenDisplay::bindVulkanBackend() {
    auto* qvkInst = window()->vulkanInstance();
    if (! qvkInst || ! qvkInst->isValid()) {
        qCWarning(lcWD, "no valid QVulkanInstance on window");
        return false;
    }

    auto* rif = window()->rendererInterface();
    if (! rif) return false;

    // VulkanInstanceResource returns VkInstance (not QVulkanInstance*).
    // Qt's getResource returns a pointer TO the Vulkan handle, not the
    // handle itself. Dereference to get the actual VkPhysicalDevice / VkDevice.
    auto* pPhysDev = static_cast<VkPhysicalDevice*>(
        rif->getResource(window(), QSGRendererInterface::PhysicalDeviceResource));
    auto* pDevice =
        static_cast<VkDevice*>(rif->getResource(window(), QSGRendererInterface::DeviceResource));

    if (! pPhysDev || ! pDevice) {
        qCWarning(lcWD,
                  "Vulkan API but missing resources "
                  "(phys=%p dev=%p)",
                  static_cast<void*>(pPhysDev),
                  static_cast<void*>(pDevice));
        return false;
    }

    VkPhysicalDevice physDev = *pPhysDev;
    VkDevice         device  = *pDevice;

#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    auto* qfip = static_cast<uint32_t*>(
        rif->getResource(window(), QSGRendererInterface::GraphicsQueueFamilyIndexResource));
    uint32_t qfi = qfip ? *qfip : 0;
#else
    uint32_t qfi = 0;
#endif

    VkInstance vkInstance = qvkInst->vkInstance();

    // Resolve the global vkGetInstanceProcAddr.
    auto rawGIPA = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        qvkInst->getInstanceProcAddr("vkGetInstanceProcAddr"));
    if (! rawGIPA) {
        qCWarning(lcWD, "failed to resolve vkGetInstanceProcAddr");
        return false;
    }

    waywallen_vk_ctx_t vk_ctx {};
    vk_ctx.instance                  = reinterpret_cast<void*>(vkInstance);
    vk_ctx.physical_device           = reinterpret_cast<void*>(physDev);
    vk_ctx.device                    = reinterpret_cast<void*>(device);
    vk_ctx.queue_family_index        = qfi;
    vk_ctx.vk_get_instance_proc_addr = reinterpret_cast<void* (*)(void*, const char*)>(rawGIPA);

#ifdef WW_HAVE_VULKAN
    // Shadow updates and retirement must use Qt's exact graphics queue.
    // A queue-family/index guess is not a synchronization contract.
    auto* pQueue = static_cast<VkQueue*>(
        rif->getResource(window(), QSGRendererInterface::CommandQueueResource));
    if (! pQueue || *pQueue == VK_NULL_HANDLE) {
        qCWarning(lcWD, "Qt did not expose its Vulkan command queue");
        return false;
    }
#endif

    int rc = waywallen_display_bind_vulkan(displayHandle(), &vk_ctx);
    if (rc != WAYWALLEN_OK) {
        qCWarning(lcWD, "bind_vulkan failed: %d", rc);
        return false;
    }

#ifdef WW_HAVE_VULKAN
    m_vkInstance = reinterpret_cast<void*>(vkInstance);
    m_vkPhys     = reinterpret_cast<void*>(physDev);
    m_vkDevice   = reinterpret_cast<void*>(device);
    m_vkQfi      = qfi;
    m_vkGipa     = reinterpret_cast<void* (*)(void*, const char*)>(rawGIPA);
    m_vkQueue    = reinterpret_cast<void*>(*pQueue);
#endif

    qCInfo(lcWD, "bound Vulkan backend, device=%p", reinterpret_cast<void*>(device));
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
        connect(this, &QQuickItem::windowChanged, this, &WaywallenDisplay::onWindowReady);
    }
}

void WaywallenDisplay::setupDBusWatcher() {
    // Optional: if there is no session bus (headless, TTY, container
    // without DBus), the exponential-backoff reconnect stays as the
    // fallback and we simply don't get the fast path.
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (! bus.isConnected()) {
        qCInfo(lcWD, "session bus unavailable — DBus reconnect fast-path disabled");
        return;
    }

    // NameOwnerChanged — fires when the daemon claims or releases
    // org.waywallen.waywallen.Daemon. The "new owner is non-empty" case means the
    // daemon just (re)appeared; reconnect now instead of waiting for the
    // local backoff timer.
    const bool okNoc = bus.connect(QStringLiteral("org.freedesktop.DBus"),
                                   QStringLiteral("/org/freedesktop/DBus"),
                                   QStringLiteral("org.freedesktop.DBus"),
                                   QStringLiteral("NameOwnerChanged"),
                                   QStringLiteral("sss"),
                                   this,
                                   SLOT(onDaemonNameOwnerChanged(QString, QString, QString)));
    if (! okNoc) {
        qCWarning(lcWD, "failed to subscribe to NameOwnerChanged");
    }

    // Also subscribe directly to the daemon's Ready signal as a second
    // cue; on some timings NameOwnerChanged is delivered before the
    // server is fully serving requests, and Ready is emitted only once
    // the daemon is actually ready. Either trigger does the same thing.
    const bool okReady = bus.connect(QStringLiteral("org.waywallen.waywallen.Daemon"),
                                     QStringLiteral("/org/waywallen/waywallen/Daemon"),
                                     QStringLiteral("org.waywallen.waywallen.Daemon1"),
                                     QStringLiteral("Ready"),
                                     this,
                                     SLOT(onDaemonReadySignal()));
    if (! okReady) {
        qCWarning(lcWD, "failed to subscribe to org.waywallen.waywallen.Daemon Ready");
    }

    qCInfo(lcWD, "DBus reconnect fast-path armed");
}

void WaywallenDisplay::onDaemonNameOwnerChanged(const QString& name, const QString& oldOwner,
                                                const QString& newOwner) {
    Q_UNUSED(oldOwner);
    if (name != QStringLiteral("org.waywallen.waywallen.Daemon")) return;
    if (newOwner.isEmpty()) return; // daemon vanished — UDS disconnect handles it
    qCInfo(lcWD, "daemon appeared on session bus — requesting reconnect");
    requestReconnect();
}

void WaywallenDisplay::onDaemonReadySignal() {
    qCInfo(lcWD, "daemon Ready signal — requesting reconnect");
    requestReconnect();
}

void WaywallenDisplay::requestReconnect() {
    if (m_connState == Connected || m_connState == Handshaking) return;
    if (! m_autoReconnect) return;
    tryConnect();
}

void WaywallenDisplay::onWindowReady() {
    if (! window()) return;

    connect(window(),
            &QQuickWindow::screenChanged,
            this,
            &WaywallenDisplay::onScreenChanged,
            Qt::UniqueConnection);
    onScreenChanged(window()->screen());

    if (m_mouseForwardEnabled) {
        // installEventFilter is idempotent on the same (target, filter)
        // pair; safe to call again if windowChanged refires.
        window()->installEventFilter(this);
        m_filterInstalled = true;
    }

    if (displayHandle()) return;

    // sceneGraphInvalidated: SG is being torn down (window closing,
    // renderer reset, etc). Fires on render thread with DirectConnection.
    // We have to release GPU resources NOW — by the time this returns
    // Qt will start destroying its VkDevice / GL context.
    connect(
        window(),
        &QQuickWindow::sceneGraphInvalidated,
        this,
        [this]() {
            qCInfo(lcWD, "sceneGraphInvalidated: releasing GPU resources");
            flushPendingRelease();
#ifdef WW_HAVE_VULKAN
            {
                QMutexLocker lk(&m_pendingMutex);
                if (m_pendingVk.valid && m_pendingVk.releaseSyncobjFd >= 0) {
                    signalFrameRelease(m_pendingVk.releaseSyncobjFd,
                                       m_pendingVk.bufferGeneration,
                                       m_pendingVk.seq,
                                       "invalidated Vulkan frame");
                }
                m_pendingVk = PendingVkFrame {};
            }
#endif
            if (auto resources = takeRenderSessionResources()) resources->shutdown();
            // Notifiers live on GUI thread; this lambda is on render
            // thread. deleteLater is documented thread-safe; it
            // posts a destruction event back to the notifier's GUI
            // thread. QPointer ensures we don't double-free if
            // cleanup() also runs in parallel.
            if (m_notifier) m_notifier->deleteLater();
            if (m_notifierWrite) m_notifierWrite->deleteLater();
            m_notifier.clear();
            m_notifierWrite.clear();
            m_eglImagesValid    = false;
            m_glTexturesCreated = false;
            m_glTextures.clear();
            m_vkImagesValid = false;
            m_vkImages.clear();
            {
                QMutexLocker lk(&m_pendingMutex);
                m_presentationState.reset();
                m_preparedEglContent         = ContentSnapshot {};
                m_notifiedPresentationSerial = m_presentationSerial;
            }
            m_textureCount  = 0;
            m_activeBackend = BackendNone;
        },
        Qt::DirectConnection);

    if (! window()->isSceneGraphInitialized()) {
        // Inject Vulkan device extensions needed for DMA-BUF import
        // before the scene graph creates the VkDevice.
        auto config = window()->graphicsConfiguration();
        config.setDeviceExtensions({
            "VK_KHR_external_memory",
            "VK_KHR_external_memory_fd",
            "VK_EXT_external_memory_dma_buf",
            "VK_EXT_queue_family_foreign",
            "VK_EXT_image_drm_format_modifier",
            "VK_KHR_external_semaphore",
            "VK_KHR_external_semaphore_fd",
        });
        window()->setGraphicsConfiguration(config);
        qCInfo(lcWD, "requested DMA-BUF Vulkan device extensions");

        connect(window(),
                &QQuickWindow::sceneGraphInitialized,
                this,
                &WaywallenDisplay::tryConnect,
                Qt::UniqueConnection);
        return;
    }
    tryConnect();
}

void WaywallenDisplay::onScreenChanged(QScreen* screen) {
    if (screen) {
        connect(screen,
                &QScreen::refreshRateChanged,
                this,
                &WaywallenDisplay::pushSizeUpdate,
                Qt::UniqueConnection);
    }
    if (displayHandle()) m_updateSizeTimer.start();
}

void WaywallenDisplay::tryConnect() {
    if (displayHandle()) return;
    setConnState(Connecting);

    waywallen_display_callbacks_t cb {};
    cb.on_binding_ready         = c_on_binding_ready;
    cb.on_textures_releasing    = c_on_textures_releasing;
    cb.on_composition_config    = c_on_composition_config;
    cb.on_frame_ready           = c_on_frame_ready;
    cb.on_presentation_snapshot = c_on_presentation_snapshot;
    cb.on_presentation_state    = c_on_presentation_state;
    cb.on_disconnected          = c_on_disconnected;
    cb.user_data                = this;

    auto resources     = std::make_shared<RenderSessionResources>();
    resources->display = waywallen_display_new(&cb);
    if (! resources->display) {
        qCWarning(lcWD, "waywallen_display_new() failed");
        setConnState(Error);
        // No internal retry — DBus NameOwnerChanged / Ready will drive
        // the next attempt when the daemon (re-)appears.
        return;
    }
    {
        QMutexLocker lock(&m_resourcesMutex);
        m_renderResources = resources;
    }
    auto* display = resources->display;

    if (waywallen_display_set_presentation_caps(display, m_presentationCapabilities) !=
        WAYWALLEN_OK) {
        qCWarning(lcWD, "failed to set presentation capabilities");
        waywallen_display_free(display);
        resources->display = nullptr;
        QMutexLocker lock(&m_resourcesMutex);
        if (m_renderResources == resources) m_renderResources.reset();
        setConnState(Error);
        return;
    }
    if (waywallen_display_set_window_state(display, m_windowStateFlags) != WAYWALLEN_OK) {
        qCWarning(lcWD, "failed to set initial window state");
        waywallen_display_free(display);
        resources->display = nullptr;
        QMutexLocker lock(&m_resourcesMutex);
        if (m_renderResources == resources) m_renderResources.reset();
        setConnState(Error);
        return;
    }
    m_windowStateFlagsDirty = false;

    // cleanup() removed the event filter on the prior session's
    // teardown; reinstall it now so mouse events resume forwarding
    // after a daemon-restart reconnect. Idempotent — Qt deduplicates
    // (target, filter) pairs.
    if (m_mouseForwardEnabled && ! m_filterInstalled && window()) {
        window()->installEventFilter(this);
        m_filterInstalled = true;
    }

    // Auto-detect Qt's graphics API and bind the matching backend.
    m_activeBackend = BackendNone;
    if (window()) {
        auto* rif = window()->rendererInterface();
        if (rif) {
            auto api = rif->graphicsApi();
            qCInfo(lcWD, "Qt graphics API: %d", int(api));

            if (api == QSGRendererInterface::OpenGL) {
                if (bindEglBackend()) m_activeBackend = BackendEGL;
            } else if (api == QSGRendererInterface::Vulkan) {
                if (bindVulkanBackend()) m_activeBackend = BackendVulkan;
            } else {
                qCWarning(lcWD, "unsupported graphics API: %d", int(api));
            }
        }
    }

    if (m_activeBackend == BackendNone) {
        qCWarning(lcWD, "no backend bound — textures will not be imported");
    }

    const QByteArray                  sockPath   = m_socketPath.toUtf8();
    const QByteArray                  name       = m_displayName.toUtf8();
    const QByteArray                  instanceId = effectiveInstanceId().toUtf8();
    const uint32_t                    refreshMhz = screenRefreshMhz();
    const waywallen_display_metrics_t metrics {
        static_cast<uint32_t>(m_displayWidth),
        static_cast<uint32_t>(m_displayHeight),
        refreshMhz,
    };
    int rc =
        waywallen_display_begin_connect(display,
                                        sockPath.isEmpty() ? nullptr : sockPath.constData(),
                                        name.constData(),
                                        instanceId.isEmpty() ? nullptr : instanceId.constData(),
                                        &metrics);

    if (rc != WAYWALLEN_OK) {
        qCWarning(lcWD, "begin_connect failed: %d (waiting for daemon DBus signal)", rc);
        waywallen_display_free(display);
        resources->display = nullptr;
        QMutexLocker lock(&m_resourcesMutex);
        if (m_renderResources == resources) m_renderResources.reset();
        setConnState(Disconnected);
        return;
    }

    // begin_connect carries these dims to the daemon as part of
    // register_display, so seed the dedupe so a same-size resize
    // post-handshake is a no-op.
    m_lastPushedWidth      = m_displayWidth;
    m_lastPushedHeight     = m_displayHeight;
    m_lastPushedRefreshMhz = refreshMhz;

    int fd = waywallen_display_get_fd(display);
    if (fd < 0) {
        qCWarning(lcWD, "begin_connect returned no fd");
        waywallen_display_free(display);
        resources->display = nullptr;
        QMutexLocker lock(&m_resourcesMutex);
        if (m_renderResources == resources) m_renderResources.reset();
        setConnState(Disconnected);
        return;
    }

    setConnState(Handshaking);

    // Two notifiers drive the async handshake until READY. Read fires on
    // POLLIN (welcome / display_accepted), Write on POLLOUT (initial
    // connect completion + hello / register_display sends). The state
    // machine in advance_handshake decides which one to enable next.
    m_notifier      = new QSocketNotifier(fd, QSocketNotifier::Read, this);
    m_notifierWrite = new QSocketNotifier(fd, QSocketNotifier::Write, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &WaywallenDisplay::onHandshakeIO);
    connect(m_notifierWrite, &QSocketNotifier::activated, this, &WaywallenDisplay::onHandshakeIO);

    // Initial arming: write is needed for either completing connect or
    // sending hello; read is armed too in case the kernel completed
    // connect already and a welcome arrives immediately.
    m_notifier->setEnabled(true);
    m_notifierWrite->setEnabled(true);
}

void WaywallenDisplay::onHandshakeIO() {
    auto* display = displayHandle();
    if (! display) return;
    int rc = waywallen_display_advance_handshake(display);
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
            disconnect(
                m_notifier, &QSocketNotifier::activated, this, &WaywallenDisplay::onHandshakeIO);
            connect(
                m_notifier, &QSocketNotifier::activated, this, &WaywallenDisplay::onSocketReadable);
            m_notifier->setEnabled(true);
        }
        if (m_notifierWrite) {
            disconnect(m_notifierWrite,
                       &QSocketNotifier::activated,
                       this,
                       &WaywallenDisplay::onHandshakeIO);
            connect(m_notifierWrite,
                    &QSocketNotifier::activated,
                    this,
                    &WaywallenDisplay::onSocketWritable);
            m_notifierWrite->setEnabled(waywallen_display_wants_writable(display));
        }
        setStreamState(Inactive);
        setConnState(Connected);
        const auto newId = qulonglong(waywallen_display_get_display_id(display));
        if (m_displayId != newId) {
            m_displayId = newId;
            emit displayIdChanged();
        }
        if (m_lastReason != None || ! m_lastMessage.isEmpty()) {
            m_lastReason = None;
            m_lastMessage.clear();
            emit lastDisconnectChanged();
        }
        // Window may have resized while the handshake was in flight;
        // reconcile by pushing if the current dims drifted from what
        // begin_connect carried.
        if (m_displayWidth != m_lastPushedWidth || m_displayHeight != m_lastPushedHeight ||
            screenRefreshMhz() != m_lastPushedRefreshMhz) {
            m_updateSizeTimer.start();
        }
        return;
    }
    if (rc < 0) {
        handleDisconnect(rc, "handshake");
        return;
    }
    // NEED_READ / NEED_WRITE: arm the matching notifier, idle the other.
    if (m_notifier) m_notifier->setEnabled(rc == WAYWALLEN_HS_NEED_READ);
    if (m_notifierWrite) m_notifierWrite->setEnabled(rc == WAYWALLEN_HS_NEED_WRITE);
}

void WaywallenDisplay::onSocketReadable() {
    auto* display = displayHandle();
    if (! display) return;
    // EGLImage creation (EGL path) and VkImage import (Vulkan path)
    // do not require a GL context. GL textures are created lazily
    // in updatePaintNode on the render thread.
    waywallen_display_dispatch(display);
    // dispatch may have queued an outgoing message (e.g. ack_unbind
    // from handle_unbind). Re-arm POLLOUT if anything stayed queued.
    armWriteNotifier();
}

void WaywallenDisplay::onSocketWritable() {
    auto* display = displayHandle();
    if (! display) return;
    waywallen_display_handle_writable(display);
    armWriteNotifier();
}

void WaywallenDisplay::armWriteNotifier() {
    if (m_notifierWrite && displayHandle()) {
        m_notifierWrite->setEnabled(waywallen_display_wants_writable(displayHandle()));
    }
}

void WaywallenDisplay::reportFrameArmed(uint64_t generation, uint64_t seq) {
    QPointer<WaywallenDisplay> guard(this);
    QMetaObject::invokeMethod(
        this,
        [guard, generation, seq]() {
            if (! guard) return;
            auto* display = guard->displayHandle();
            if (! display) return;
            const int rc = waywallen_display_frame_release_armed(display, generation, seq);
            if (rc != WAYWALLEN_OK) {
                qCWarning(lcWD,
                          "frame_release_armed enqueue failed: generation=%llu seq=%llu rc=%d",
                          qulonglong(generation),
                          qulonglong(seq),
                          rc);
                guard->handleDisconnect(rc, "frame_release_armed enqueue failed");
                return;
            }
            guard->armWriteNotifier();
        },
        Qt::QueuedConnection);
}

void WaywallenDisplay::signalFrameRelease(int fd, uint64_t generation, uint64_t seq,
                                          const char* context) {
    if (fd < 0) return;
    const int rc = waywallen_display_signal_release_syncobj(fd);
    if (rc == WAYWALLEN_OK) {
        reportFrameArmed(generation, seq);
        return;
    }
    const QString reason =
        QStringLiteral("%1 release signal failed").arg(QString::fromUtf8(context));
    qCWarning(lcWD,
              "%s: signal release failed for generation=%llu seq=%llu rc=%d",
              context,
              qulonglong(generation),
              qulonglong(seq),
              rc);
    QPointer<WaywallenDisplay> guard(this);
    QMetaObject::invokeMethod(
        this,
        [guard, rc, reason]() {
            if (guard) guard->handleDisconnect(rc, reason.toUtf8().constData());
        },
        Qt::QueuedConnection);
}

void WaywallenDisplay::flushPendingRelease() {
    PendingEglFrame frame;
    {
        QMutexLocker lk(&m_pendingMutex);
        frame        = m_pendingEgl;
        m_pendingEgl = PendingEglFrame {};
    }
    if (frame.releaseSyncobjFd >= 0) {
        signalFrameRelease(
            frame.releaseSyncobjFd, frame.bufferGeneration, frame.seq, "skipped EGL frame");
    }
}

void WaywallenDisplay::releaseEglFrame(int releaseSyncobjFd, bool afterGpuWork, uint64_t generation,
                                       uint64_t seq) {
    if (releaseSyncobjFd < 0) return;

    auto* ctx       = QOpenGLContext::currentContext();
    auto  resources = renderSessionResources();
    if (afterGpuWork && ctx && resources && resources->eglDisplay) {
        auto createSync =
            reinterpret_cast<PFNEGLCREATESYNCKHRPROC>(ctx->getProcAddress("eglCreateSyncKHR"));
        auto destroySync =
            reinterpret_cast<PFNEGLDESTROYSYNCKHRPROC>(ctx->getProcAddress("eglDestroySyncKHR"));
        auto dupFence = reinterpret_cast<PFNEGLDUPNATIVEFENCEFDANDROIDPROC>(
            ctx->getProcAddress("eglDupNativeFenceFDANDROID"));
        if (createSync && destroySync && dupFence) {
            const EGLint attrs[] = { EGL_NONE };
            auto         display = reinterpret_cast<EGLDisplay>(resources->eglDisplay);
            EGLSyncKHR   sync    = createSync(display, EGL_SYNC_NATIVE_FENCE_ANDROID, attrs);
            if (sync != EGL_NO_SYNC_KHR) {
                ctx->extraFunctions()->glFlush();
                int syncFileFd = dupFence(display, sync);
                (void)destroySync(display, sync);
                if (syncFileFd != EGL_NO_NATIVE_FENCE_FD_ANDROID) {
                    int fallbackReleaseFd = ::dup(releaseSyncobjFd);
                    if (fallbackReleaseFd < 0) {
                        ::close(syncFileFd);
                        ctx->extraFunctions()->glFinish();
                        signalFrameRelease(releaseSyncobjFd, generation, seq, "EGL dup fallback");
                        return;
                    }
                    int rc =
                        waywallen_display_release_after_sync_file(releaseSyncobjFd, syncFileFd);
                    if (rc == WAYWALLEN_OK) {
                        ::close(fallbackReleaseFd);
                        reportFrameArmed(generation, seq);
                    } else {
                        qCWarning(lcWD, "EGL: attach release fence failed: %d", rc);
                        ctx->extraFunctions()->glFinish();
                        signalFrameRelease(
                            fallbackReleaseFd, generation, seq, "EGL fence fallback");
                    }
                    return;
                }
            }
        }

        ctx->extraFunctions()->glFinish();
    }

    signalFrameRelease(releaseSyncobjFd, generation, seq, "EGL frame");
}

void WaywallenDisplay::handleDisconnect(int errCode, const char* msg) {
    qCWarning(lcWD,
              "disconnected (err=%d msg=%s) — waiting for daemon DBus signal",
              errCode,
              msg ? msg : "(null)");
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
    auto* display = displayHandle();
    if (m_glTexturesCreated || ! m_eglImagesValid || ! display) return;

    m_glTextures.resize(static_cast<int>(m_textureCount));
    bool ok = true;
    for (uint32_t i = 0; i < m_textureCount; i++) {
        uint32_t tex = 0;
        int      rc  = waywallen_display_create_gl_texture(display, i, &tex);
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

WaywallenDisplay::EglBlitResult WaywallenDisplay::blitEglShadow(int slot, int width, int height,
                                                                bool forceReplace) {
    auto resourcesOwner = renderSessionResources();
    if (! resourcesOwner || slot < 0 || slot >= m_glTextures.size()) {
        return EglBlitResult::Failed;
    }
    auto& resources = *resourcesOwner;
    auto* ctx       = QOpenGLContext::currentContext();
    if (! ctx) return EglBlitResult::Failed;
    auto* gl = ctx->extraFunctions();
    if (! gl) return EglBlitResult::Failed;

    const int w = width;
    const int h = height;
    if (w <= 0 || h <= 0) return EglBlitResult::Failed;

    const bool replace = forceReplace || resources.eglShadowTex == 0 ||
                         resources.eglShadowFbo == 0 || resources.eglShadowW != w ||
                         resources.eglShadowH != h;
    if (replace && ! resources.discardEglCandidate()) {
        qCWarning(lcWD, "EGL: cannot discard previous shadow candidate without a GL context");
        return EglBlitResult::Failed;
    }

    GLint prevDraw = 0, prevRead = 0, prevTexture = 0;
    gl->glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDraw);
    gl->glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevRead);
    gl->glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTexture);

    GLboolean prevScissor = gl->glIsEnabled(GL_SCISSOR_TEST);
    if (prevScissor) gl->glDisable(GL_SCISSOR_TEST);

    uint targetTex = replace ? 0 : resources.eglShadowTex;
    uint targetFbo = replace ? 0 : resources.eglShadowFbo;
    if (replace) {
        gl->glGenTextures(1, &targetTex);
        gl->glGenFramebuffers(1, &targetFbo);
        if (targetTex == 0 || targetFbo == 0) {
            if (targetFbo) gl->glDeleteFramebuffers(1, &targetFbo);
            if (targetTex) gl->glDeleteTextures(1, &targetTex);
            gl->glBindTexture(GL_TEXTURE_2D, static_cast<uint>(prevTexture));
            gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prevDraw);
            gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, prevRead);
            if (prevScissor) gl->glEnable(GL_SCISSOR_TEST);
            return EglBlitResult::Failed;
        }
        gl->glBindTexture(GL_TEXTURE_2D, targetTex);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    }

    gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, targetFbo);
    gl->glFramebufferTexture2D(
        GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, targetTex, 0);
    if (resources.eglReadFbo == 0) {
        gl->glGenFramebuffers(1, &resources.eglReadFbo);
    }
    gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, resources.eglReadFbo);
    gl->glFramebufferTexture2D(
        GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_glTextures[slot], 0);

    const bool complete =
        gl->glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE &&
        gl->glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (complete) {
        gl->glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    const GLenum error = gl->glGetError();

    gl->glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);

    gl->glBindTexture(GL_TEXTURE_2D, static_cast<uint>(prevTexture));
    gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prevDraw);
    gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, prevRead);
    if (prevScissor) gl->glEnable(GL_SCISSOR_TEST);

    if (! complete || error != GL_NO_ERROR) {
        qCWarning(
            lcWD, "EGL: shadow blit failed (complete=%d glError=0x%x)", complete ? 1 : 0, error);
        if (replace) {
            gl->glDeleteFramebuffers(1, &targetFbo);
            gl->glDeleteTextures(1, &targetTex);
        }
        return complete ? EglBlitResult::FailedAfterGpuWork : EglBlitResult::Failed;
    }

    if (replace) {
        resources.eglCandidateFbo        = targetFbo;
        resources.eglCandidateTex        = targetTex;
        resources.eglCandidateW          = w;
        resources.eglCandidateH          = h;
        resources.eglCandidateHasContent = true;
        const qulonglong candidateBytes  = qulonglong(w) * qulonglong(h) * 4ull;
        const qulonglong currentBytes =
            qulonglong(resources.eglShadowW) * qulonglong(resources.eglShadowH) * 4ull;
        qCInfo(lcWD,
               "EGL shadow candidate: display=%llu %dx%d bytes=%llu resident=%llu",
               m_displayId,
               w,
               h,
               candidateBytes,
               currentBytes + candidateBytes);
        return EglBlitResult::CandidateReady;
    }
    resources.eglShadowHasContent = true;
    return EglBlitResult::CurrentUpdated;
}

// Drains m_pendingEgl and runs blitEglShadow. Scheduled by
// c_on_frame_ready via scheduleRenderJob(BeforeSynchronizingStage),
// so the imported buffer gets copied to the shadow exactly once per
// frame_ready arrival — Qt repaints driven by other dirty sources
// don't trigger a redundant blit.
void WaywallenDisplay::renderThreadBlitEgl() {
    PendingEglFrame frame;
    ContentSnapshot incoming;
    bool            replacesPresentation = false;
    {
        QMutexLocker lk(&m_pendingMutex);
        if (! m_pendingEgl.valid) return;
        frame        = m_pendingEgl;
        m_pendingEgl = PendingEglFrame {};
        (void)m_presentationState.incomingFor(frame.bufferGeneration, incoming);
        replacesPresentation = m_presentationState.sourceChangesWith(frame.bufferGeneration);
    }
    if (! incoming.valid || m_activeBackend != BackendEGL || ! m_eglImagesValid) {
        releaseEglFrame(frame.releaseSyncobjFd, false, frame.bufferGeneration, frame.seq);
        return;
    }
    // EGLImage → GL texture binding is render-thread work; safe here.
    if (! m_glTexturesCreated) ensureGlTextures();
    if (! m_glTexturesCreated || frame.slot < 0 || frame.slot >= m_glTextures.size() ||
        incoming.width <= 0 || incoming.height <= 0) {
        releaseEglFrame(frame.releaseSyncobjFd, false, frame.bufferGeneration, frame.seq);
        return;
    }
    const EglBlitResult result =
        blitEglShadow(frame.slot, incoming.width, incoming.height, replacesPresentation);
    if (result == EglBlitResult::CurrentUpdated) {
        commitPresentedContent(frame.bufferGeneration,
                               incoming.width,
                               incoming.height,
                               incoming.fourcc,
                               incoming.config);
        releaseEglFrame(frame.releaseSyncobjFd, true, frame.bufferGeneration, frame.seq);
    } else if (result == EglBlitResult::CandidateReady) {
        {
            QMutexLocker lk(&m_pendingMutex);
            m_preparedEglContent = incoming;
        }
        releaseEglFrame(frame.releaseSyncobjFd, true, frame.bufferGeneration, frame.seq);
    } else if (result == EglBlitResult::FailedAfterGpuWork) {
        releaseEglFrame(frame.releaseSyncobjFd, true, frame.bufferGeneration, frame.seq);
    } else {
        releaseEglFrame(frame.releaseSyncobjFd, false, frame.bufferGeneration, frame.seq);
    }
}

// ---------------------------------------------------------------------------
// Scene graph
// ---------------------------------------------------------------------------

QSGNode* WaywallenDisplay::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) {
    auto  resourcesOwner = renderSessionResources();
    auto* resources      = resourcesOwner.get();
    // Run library-deferred pool destructions on the render thread,
    // where (a) Qt's GL context is current for glDeleteTextures (EGL
    // path) and (b) we can guarantee no in-flight vkQueueSubmit on
    // the blitter is still referencing the released VkImages (Vulkan
    // path). The library itself never calls vkDeviceWaitIdle from the
    // I/O thread anymore — that's what was racing with Qt's RHI on
    // radv and surfacing as VK_ERROR_DEVICE_LOST during rapid
    // wallpaper switches.
    if (resources && resources->display) {
#ifdef WW_HAVE_VULKAN
        // Vulkan: skip drain when the blitter's fence is in-flight —
        // its cmd buffer may still be reading the most recently
        // released pool's VkImage (only happens after a post-submit
        // timeout; in steady state fence is cleared between blits).
        // Next iteration's pre-submit wait will clear it and we
        // drain then.
        const bool blitterBusy = resources->vkBlitterInited && resources->vkBlitter.fence_armed;
        if (! blitterBusy) {
            (void)waywallen_display_drain(resources->display);
        }
#else
        (void)waywallen_display_drain(resources->display);
#endif
    }

    auto* rootNode = dynamic_cast<PresentationNode*>(oldNode);
    if (! rootNode && oldNode) {
        delete oldNode;
        oldNode = nullptr;
    }
    auto ensureRootNode = [&]() -> PresentationNode* {
        if (rootNode) return rootNode;
        if (! window()) return nullptr;
        rootNode = PresentationNode::create(window());
        oldNode  = rootNode;
        return rootNode;
    };
    auto scheduleSessionFailure = [this](const QString& reason) {
        QPointer<WaywallenDisplay> guard(this);
        QMetaObject::invokeMethod(
            this,
            [guard, reason]() {
                if (guard) guard->handleDisconnect(-EIO, reason.toUtf8().constData());
            },
            Qt::QueuedConnection);
    };

    ContentSnapshot eglCandidateContent;
    {
        QMutexLocker lk(&m_pendingMutex);
        if (m_preparedEglContent.valid) {
            ContentSnapshot latest;
            if (m_presentationState.incomingFor(m_preparedEglContent.bufferGeneration, latest)) {
                eglCandidateContent = latest;
            }
            m_preparedEglContent = ContentSnapshot {};
        }
    }

#ifdef WW_HAVE_VULKAN
    PendingVkFrame  frame;
    ContentSnapshot incoming;
    ContentSnapshot vkCandidateContent;
    bool            replacesPresentation = false;
    {
        QMutexLocker lk(&m_pendingMutex);
        frame       = m_pendingVk;
        m_pendingVk = PendingVkFrame {};
        if (frame.valid) {
            (void)m_presentationState.incomingFor(frame.bufferGeneration, incoming);
            replacesPresentation = m_presentationState.sourceChangesWith(frame.bufferGeneration);
        }
    }

    const bool canBlitVk = frame.valid && incoming.valid && m_activeBackend == BackendVulkan &&
                           m_vkImagesValid && frame.slot >= 0 && frame.slot < m_vkImages.size() &&
                           incoming.width > 0 && incoming.height > 0;
    if (frame.valid && ! canBlitVk) {
        if (frame.releaseSyncobjFd >= 0) {
            signalFrameRelease(
                frame.releaseSyncobjFd, frame.bufferGeneration, frame.seq, "unusable Vulkan frame");
        }
    } else if (canBlitVk) {
        if (! resources || ! resources->vkBlitterInited) {
            if (! resources) {
                if (frame.releaseSyncobjFd >= 0) {
                    signalFrameRelease(frame.releaseSyncobjFd,
                                       frame.bufferGeneration,
                                       frame.seq,
                                       "Vulkan frame without resources");
                }
                frame.valid = false;
            }
        }
        if (frame.valid && ! resources->vkBlitterInited) {
            int rc =
                ww_vk_blitter_init(&resources->vkBlitter,
                                   reinterpret_cast<VkInstance>(m_vkInstance),
                                   reinterpret_cast<VkPhysicalDevice>(m_vkPhys),
                                   reinterpret_cast<VkDevice>(m_vkDevice),
                                   m_vkQfi,
                                   reinterpret_cast<VkQueue>(m_vkQueue),
                                   reinterpret_cast<ww_vk_get_instance_proc_addr_fn>(m_vkGipa));
            if (rc != 0) {
                qCWarning(
                    lcWD, "vk blitter init failed (%d); Vulkan path disabled this session", rc);
                if (frame.releaseSyncobjFd >= 0) {
                    signalFrameRelease(frame.releaseSyncobjFd,
                                       frame.bufferGeneration,
                                       frame.seq,
                                       "Vulkan blitter init failure");
                }
                frame.valid = false;
            } else {
                resources->vkBlitterInited = true;
            }
        }

        if (frame.valid) {
            auto      imported       = reinterpret_cast<VkImage>(m_vkImages[frame.slot]);
            auto      acquireSem     = reinterpret_cast<VkSemaphore>(frame.acquireSem);
            bool      candidateReady = false;
            bool      releaseArmed   = false;
            const int rc             = ww_vk_blitter_prepare(&resources->vkBlitter,
                                                             imported,
                                                             static_cast<uint32_t>(incoming.width),
                                                             static_cast<uint32_t>(incoming.height),
                                                             incoming.fourcc,
                                                             replacesPresentation,
                                                             acquireSem,
                                                             frame.releaseSyncobjFd,
                                                             &candidateReady,
                                                             &releaseArmed);
            if (releaseArmed) reportFrameArmed(frame.bufferGeneration, frame.seq);
            if (rc == 0) {
                if (candidateReady) {
                    vkCandidateContent = incoming;
                    qCInfo(
                        lcWD,
                        "Vulkan shadow candidate: display=%llu %dx%d bytes=%llu resident=%llu",
                        m_displayId,
                        incoming.width,
                        incoming.height,
                        qulonglong(ww_vk_blitter_candidate_allocation_size(&resources->vkBlitter)),
                        qulonglong(ww_vk_blitter_shadow_allocation_size(&resources->vkBlitter) +
                                   ww_vk_blitter_candidate_allocation_size(&resources->vkBlitter)));
                } else {
                    commitPresentedContent(frame.bufferGeneration,
                                           incoming.width,
                                           incoming.height,
                                           incoming.fourcc,
                                           incoming.config);
                }
            } else if (! releaseArmed) {
                bool drainedRelease = false;
                (void)ww_vk_blitter_drain_pending_release(&resources->vkBlitter, &drainedRelease);
                if (drainedRelease) reportFrameArmed(frame.bufferGeneration, frame.seq);
                scheduleSessionFailure(QStringLiteral("Vulkan frame release could not be armed"));
            } else if (ww_vk_blitter_candidate(&resources->vkBlitter) != VK_NULL_HANDLE &&
                       ww_vk_blitter_discard_candidate(&resources->vkBlitter) != 0) {
                qCCritical(lcWD, "Vulkan shadow candidate remains in flight; ending session");
                scheduleSessionFailure(QStringLiteral("Vulkan shadow candidate did not drain"));
            }
        }
    }
#endif

    if (resources && resources->eglCandidateTex != 0) {
        const bool validCandidate = m_activeBackend == BackendEGL && eglCandidateContent.valid &&
                                    resources->eglCandidateHasContent;
        if (! validCandidate || ! window()) {
            if (! resources->discardEglCandidate()) {
                qCCritical(lcWD, "EGL shadow candidate could not be discarded");
                scheduleSessionFailure(QStringLiteral("EGL shadow candidate cleanup failed"));
            }
        } else if (auto* candidateNode = ensureRootNode()) {
            const QSize size(eglCandidateContent.width, eglCandidateContent.height);
            QSGTexture* wrapper = QNativeInterface::QSGOpenGLTexture::fromNative(
                resources->eglCandidateTex, window(), size, QQuickWindow::TextureHasAlphaChannel);
            if (! wrapper) {
                qCWarning(lcWD, "EGL candidate native texture wrapper creation failed");
                (void)resources->discardEglCandidate();
            } else {
                candidateNode->installTexture(wrapper,
                                              PresentationNode::TextureBackend::OpenGL,
                                              static_cast<quintptr>(resources->eglCandidateTex),
                                              size);
                if (! resources->commitEglCandidate()) {
                    qCCritical(lcWD, "EGL candidate commit failed after wrapper replacement");
                    delete rootNode;
                    rootNode = nullptr;
                    oldNode  = nullptr;
                    scheduleSessionFailure(QStringLiteral("EGL shadow commit failed"));
                } else {
                    qCInfo(lcWD,
                           "EGL shadow committed: display=%llu resident=%llu",
                           m_displayId,
                           qulonglong(resources->eglShadowW) * qulonglong(resources->eglShadowH) *
                               4ull);
                    commitPresentedContent(eglCandidateContent.bufferGeneration,
                                           eglCandidateContent.width,
                                           eglCandidateContent.height,
                                           eglCandidateContent.fourcc,
                                           eglCandidateContent.config);
                }
            }
        } else {
            (void)resources->discardEglCandidate();
        }
    }

#ifdef WW_HAVE_VULKAN
    if (resources && vkCandidateContent.valid && resources->vkBlitterInited &&
        ww_vk_blitter_candidate_has_content(&resources->vkBlitter)) {
        if (! window()) {
            (void)ww_vk_blitter_discard_candidate(&resources->vkBlitter);
        } else if (auto* candidateNode = ensureRootNode()) {
            const QSize size(vkCandidateContent.width, vkCandidateContent.height);
            const auto  image   = ww_vk_blitter_candidate(&resources->vkBlitter);
            QSGTexture* wrapper = QNativeInterface::QSGVulkanTexture::fromNative(
                image,
                ww_vk_blitter_shadow_layout(&resources->vkBlitter),
                window(),
                size,
                QQuickWindow::TextureHasAlphaChannel);
            if (! wrapper) {
                qCWarning(lcWD, "Vulkan candidate native texture wrapper creation failed");
                (void)ww_vk_blitter_discard_candidate(&resources->vkBlitter);
            } else {
                candidateNode->installTexture(wrapper,
                                              PresentationNode::TextureBackend::Vulkan,
                                              reinterpret_cast<quintptr>(image),
                                              size);
                const VkResult result = ww_vk_blitter_commit_candidate(&resources->vkBlitter);
                if (result != VK_SUCCESS) {
                    qCCritical(lcWD,
                               "Vulkan candidate commit failed after wrapper replacement: %d",
                               int(result));
                    delete rootNode;
                    rootNode = nullptr;
                    oldNode  = nullptr;
                    scheduleSessionFailure(QStringLiteral("Vulkan shadow commit failed"));
                } else {
                    qCInfo(lcWD,
                           "Vulkan shadow committed: display=%llu resident=%llu",
                           m_displayId,
                           qulonglong(ww_vk_blitter_shadow_allocation_size(&resources->vkBlitter)));
                    commitPresentedContent(vkCandidateContent.bufferGeneration,
                                           vkCandidateContent.width,
                                           vkCandidateContent.height,
                                           vkCandidateContent.fourcc,
                                           vkCandidateContent.config);
                }
            }
        } else {
            (void)ww_vk_blitter_discard_candidate(&resources->vkBlitter);
        }
    }
#endif

    ContentSnapshot presented;
    {
        QMutexLocker lk(&m_pendingMutex);
        presented = m_presentationState.presented();
    }

    const bool hasTexture =
        // EGL gate: only expose the shadow once at least one frame has
        // been blitted into it, otherwise we'd sample uninitialized
        // GPU memory. After the first frame the shadow stays valid
        // across pool transitions, which is what gives the EGL path
        // the same "keep last frame on switch" continuity the Vulkan
        // path has.
        (resources && presented.valid && m_activeBackend == BackendEGL &&
         resources->eglShadowTex != 0 && resources->eglShadowHasContent)
#ifdef WW_HAVE_VULKAN
        // Gate Vulkan sampling on a shadow populated by a completed copy.
        || (resources && presented.valid && m_activeBackend == BackendVulkan &&
            resources->vkBlitterInited &&
            ww_vk_blitter_shadow(&resources->vkBlitter) != VK_NULL_HANDLE &&
            ww_vk_blitter_shadow_has_content(&resources->vkBlitter))
#endif
        ;

    if (! hasTexture || ! window()) {
        delete rootNode;
        return nullptr;
    }

    rootNode = ensureRootNode();
    if (! rootNode) return nullptr;
    auto* clearNode = rootNode->clearNode;
    auto* xformNode = rootNode->transformNode;
    auto* node      = rootNode->imageNode;

    const QRectF bounds = boundingRect();
    clearNode->setRect(bounds);
    clearNode->setColor(presented.config.clearColor);

    const QSize texSize(presented.width, presented.height);

    if (m_activeBackend == BackendEGL) {
        const auto handle = static_cast<quintptr>(resources->eglShadowTex);
        if (! rootNode->wraps(PresentationNode::TextureBackend::OpenGL, handle, texSize)) {
            QSGTexture* wrapper = QNativeInterface::QSGOpenGLTexture::fromNative(
                resources->eglShadowTex, window(), texSize, QQuickWindow::TextureHasAlphaChannel);
            if (! wrapper) {
                delete rootNode;
                return nullptr;
            }
            rootNode->installTexture(
                wrapper, PresentationNode::TextureBackend::OpenGL, handle, texSize);
        }
    } else if (m_activeBackend == BackendVulkan) {
#ifdef WW_HAVE_VULKAN
        const auto image  = ww_vk_blitter_shadow(&resources->vkBlitter);
        const auto handle = reinterpret_cast<quintptr>(image);
        if (! rootNode->wraps(PresentationNode::TextureBackend::Vulkan, handle, texSize)) {
            QSGTexture* wrapper = QNativeInterface::QSGVulkanTexture::fromNative(
                image,
                ww_vk_blitter_shadow_layout(&resources->vkBlitter),
                window(),
                texSize,
                QQuickWindow::TextureHasAlphaChannel);
            if (! wrapper) {
                delete rootNode;
                return nullptr;
            }
            rootNode->installTexture(
                wrapper, PresentationNode::TextureBackend::Vulkan, handle, texSize);
        }
#endif
    }

    if (presented.config.sourceRect.width() > 0 && presented.config.sourceRect.height() > 0) {
        node->setSourceRect(presented.config.sourceRect);
    } else {
        node->setSourceRect(QRectF(0, 0, presented.width, presented.height));
    }

    if (presented.config.destRect.width() > 0 && presented.config.destRect.height() > 0 &&
        m_displayWidth > 0 && m_displayHeight > 0) {
        const qreal sx = bounds.width() / qreal(m_displayWidth);
        const qreal sy = bounds.height() / qreal(m_displayHeight);
        node->setRect(QRectF(presented.config.destRect.x() * sx,
                             presented.config.destRect.y() * sy,
                             presented.config.destRect.width() * sx,
                             presented.config.destRect.height() * sy));
    } else {
        node->setRect(bounds);
    }

    // Build the rotation matrix: rotate the pre-rotation dest rect
    // (sized boundsH × boundsW for 90°/270°, boundsW × boundsH for
    // 0°/180°) around the post-rotation display center so it lands
    // back inside the item's bounds. Qt's QMatrix4x4 rotation around
    // +Z is visually CW in screen coords (Y points down), which is
    // exactly how `Rotation::Cw*` is meant to be displayed.
    QMatrix4x4 mat;
    if (presented.config.transform != 0) {
        const qreal w        = bounds.width();
        const qreal h        = bounds.height();
        const bool swap_dims = (presented.config.transform == 1 || presented.config.transform == 3);
        const qreal pre_w    = swap_dims ? h : w;
        const qreal pre_h    = swap_dims ? w : h;
        const float angle    = static_cast<float>(presented.config.transform * 90u);
        mat.translate(static_cast<float>(w / 2.0), static_cast<float>(h / 2.0));
        mat.rotate(angle, 0.0f, 0.0f, 1.0f);
        mat.translate(static_cast<float>(-pre_w / 2.0), static_cast<float>(-pre_h / 2.0));
    }
    if (xformNode->matrix() != mat) {
        xformNode->setMatrix(mat);
        xformNode->markDirty(QSGNode::DirtyMatrix);
    }

    return rootNode;
}
