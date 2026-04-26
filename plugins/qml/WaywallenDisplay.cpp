#include "WaywallenDisplay.hpp"

#include <waywallen_display.h>

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDebug>
#include <QLoggingCategory>
#include <QQuickGraphicsConfiguration>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSGSimpleTextureNode>
#include <QtGui/qopenglcontext_platform.h>
#include <QtQuick/qsgtexture_platform.h>

Q_LOGGING_CATEGORY(lcWD, "waywallen.display")

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
}

void WaywallenDisplay::c_on_frame_ready(void *ud,
                                        const waywallen_frame_t *f) {
    auto *self = static_cast<WaywallenDisplay *>(ud);
    self->flushPendingRelease();
    self->m_framesReceived++;
    self->m_currentSlot = static_cast<int>(f->buffer_index);
    self->m_pendingReleaseIdx = f->buffer_index;
    self->m_pendingReleaseSeq = f->seq;
    self->m_pendingRelease = true;
    emit self->framesReceivedChanged();
    self->update();
}

void WaywallenDisplay::c_on_disconnected(void *ud, int err,
                                         const char *msg) {
    auto *self = static_cast<WaywallenDisplay *>(ud);
    // If m_display was nulled (e.g. by sceneGraphInvalidated on the
    // render thread), skip — we're already tearing down.
    if (!self->m_display) return;
    self->handleDisconnect(err, msg);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

WaywallenDisplay::WaywallenDisplay(QQuickItem *parent)
    : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
    waywallen_display_set_log_callback(qtLogBridge, nullptr);
}

WaywallenDisplay::~WaywallenDisplay() {
    // cleanup() may already have been called by sceneGraphInvalidated.
    // It's safe to call again — it checks m_display for null.
    cleanup();
}

void WaywallenDisplay::cleanup() {
    delete m_notifier;
    m_notifier = nullptr;
    delete m_notifierWrite;
    m_notifierWrite = nullptr;

    if (m_display) {
        waywallen_display_disconnect(m_display);
        waywallen_display_destroy(m_display);
        m_display = nullptr;
    }

    m_eglImagesValid = false;
    m_glTexturesCreated = false;
    m_glTextures.clear();
    m_vkImagesValid = false;
    m_vkImages.clear();
    m_currentSlot = -1;
    m_textureCount = 0;
    m_pendingRelease = false;
    m_activeBackend = BackendNone;
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

void WaywallenDisplay::setDisplayWidth(int w) {
    if (m_displayWidth == w) return;
    m_displayWidth = w;
    emit displayWidthChanged();
}

void WaywallenDisplay::setDisplayHeight(int h) {
    if (m_displayHeight == h) return;
    m_displayHeight = h;
    emit displayHeightChanged();
}

void WaywallenDisplay::setClearColor(const QColor &color) {
    if (m_clearColor == color) return;
    m_clearColor = color;
    emit clearColorChanged();
    update();
}

void WaywallenDisplay::setAutoReconnect(bool enabled) {
    if (m_autoReconnect == enabled) return;
    m_autoReconnect = enabled;
    emit autoReconnectChanged();
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
    if (!window() || m_display) return;

    // Release GPU resources (VkImage, EGLImage, GL textures) before Qt
    // destroys its Vulkan device / GL context. This signal fires on the
    // render thread — only touch the C library handle, not Qt objects.
    connect(window(), &QQuickWindow::sceneGraphInvalidated, this,
            [this]() {
                qCInfo(lcWD, "sceneGraphInvalidated: releasing GPU resources");
                // Destroy the C library handle (which releases VkImage,
                // EGLImage, semaphores etc.) while Qt's device is still alive.
                // Null out the handle first so the on_disconnected callback
                // (which fires from disconnect) becomes a no-op.
                auto *d = m_display;
                m_display = nullptr;
                if (d) {
                    waywallen_display_disconnect(d);
                    waywallen_display_destroy(d);
                }
                m_eglImagesValid = false;
                m_glTexturesCreated = false;
                m_glTextures.clear();
                m_vkImagesValid = false;
                m_vkImages.clear();
                m_currentSlot = -1;
                m_textureCount = 0;
                m_pendingRelease = false;
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
    int rc = waywallen_display_begin_connect(
        m_display,
        sockPath.isEmpty() ? nullptr : sockPath.constData(),
        name.constData(),
        static_cast<uint32_t>(m_displayWidth),
        static_cast<uint32_t>(m_displayHeight),
        60000);

    if (rc != WAYWALLEN_OK) {
        qCWarning(lcWD, "begin_connect failed: %d (waiting for daemon DBus signal)", rc);
        waywallen_display_destroy(m_display);
        m_display = nullptr;
        setConnState(Disconnected);
        return;
    }

    int fd = waywallen_display_get_fd(m_display);
    if (fd < 0) {
        qCWarning(lcWD, "begin_connect returned no fd");
        waywallen_display_destroy(m_display);
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

    // First step always involves writing (either completing connect or
    // sending hello). Read is armed in case the kernel completed connect
    // already and a welcome arrives immediately.
    m_notifier->setEnabled(true);
    m_notifierWrite->setEnabled(true);
}

void WaywallenDisplay::onHandshakeIO() {
    if (!m_display) return;
    int rc = waywallen_display_advance_handshake(m_display);
    if (rc == WAYWALLEN_HS_DONE) {
        qCInfo(lcWD, "handshake complete");
        // Tear down the write notifier; post-handshake dispatch is
        // read-only. Reuse the existing read notifier but redirect it
        // to onSocketReadable.
        delete m_notifierWrite;
        m_notifierWrite = nullptr;
        if (m_notifier) {
            disconnect(m_notifier, &QSocketNotifier::activated,
                       this, &WaywallenDisplay::onHandshakeIO);
            connect(m_notifier, &QSocketNotifier::activated,
                    this, &WaywallenDisplay::onSocketReadable);
            m_notifier->setEnabled(true);
        }
        setStreamState(Inactive);
        setConnState(Connected);
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
}

void WaywallenDisplay::flushPendingRelease() {
    if (m_pendingRelease && m_display) {
        waywallen_display_release_frame(
            m_display, m_pendingReleaseIdx, m_pendingReleaseSeq);
        m_pendingRelease = false;
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

// ---------------------------------------------------------------------------
// Scene graph
// ---------------------------------------------------------------------------

QSGNode *WaywallenDisplay::updatePaintNode(QSGNode *oldNode,
                                           UpdatePaintNodeData *) {
    // EGL path: create GL textures lazily on the render thread.
    if (m_activeBackend == BackendEGL && m_eglImagesValid
        && !m_glTexturesCreated) {
        ensureGlTextures();
    }

    const bool hasTexture =
        (m_activeBackend == BackendEGL && m_glTexturesCreated
         && m_currentSlot >= 0 && m_currentSlot < m_glTextures.size())
        || (m_activeBackend == BackendVulkan && m_vkImagesValid
            && m_currentSlot >= 0 && m_currentSlot < m_vkImages.size());

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

    QSize texSize(m_texWidth, m_texHeight);
    QSGTexture *sgTex = nullptr;

    if (m_activeBackend == BackendEGL) {
        uint glTex = m_glTextures[m_currentSlot];
        sgTex = QNativeInterface::QSGOpenGLTexture::fromNative(
            glTex, window(), texSize,
            QQuickWindow::TextureHasAlphaChannel);
    } else if (m_activeBackend == BackendVulkan) {
#if QT_CONFIG(vulkan)
        auto vkImage = reinterpret_cast<VkImage>(m_vkImages[m_currentSlot]);
        sgTex = QNativeInterface::QSGVulkanTexture::fromNative(
            vkImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
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

    node->setRect(boundingRect());
    return node;
}
