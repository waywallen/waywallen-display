#include "WaywallenDisplay.hpp"

#include <waywallen_display.h>

#include <QDebug>
#include <QQuickWindow>
#include <QSGNode>
#include <QSGRendererInterface>
#include <QSGSimpleTextureNode>
#include <QtQuick/qsgtexture_platform.h>

// ---------------------------------------------------------------------------
// C callback trampolines
// ---------------------------------------------------------------------------

void WaywallenDisplay::c_on_textures_ready(void *ud,
                                           const waywallen_textures_t *t) {
    auto *self = static_cast<WaywallenDisplay *>(ud);
    self->m_textureCount = t->count;
    self->m_texWidth = static_cast<int>(t->tex_width);
    self->m_texHeight = static_cast<int>(t->tex_height);

    if (t->backend == WAYWALLEN_BACKEND_EGL && t->gl_textures) {
        self->m_glTextures.resize(static_cast<int>(t->count));
        for (uint32_t i = 0; i < t->count; i++)
            self->m_glTextures[static_cast<int>(i)] = t->gl_textures[i];
        self->m_texturesValid = true;
        self->setStatus(Bound);
    } else {
        self->m_glTextures.clear();
        self->m_texturesValid = false;
        self->setStatus(Idle);
    }
}

void WaywallenDisplay::c_on_textures_releasing(void *ud,
                                               const waywallen_textures_t *t) {
    auto *self = static_cast<WaywallenDisplay *>(ud);
    Q_UNUSED(t);
    self->flushPendingRelease();
    self->m_texturesValid = false;
    self->m_glTextures.clear();
    self->m_currentSlot = -1;
    self->m_textureCount = 0;
    self->setStatus(Idle);
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
    // Pick up server-pushed clear color.
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
    self->handleDisconnect(err, msg);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

WaywallenDisplay::WaywallenDisplay(QQuickItem *parent)
    : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
}

WaywallenDisplay::~WaywallenDisplay() {
    cleanup();
    delete m_reconnectTimer;
}

void WaywallenDisplay::cleanup() {
    delete m_notifier;
    m_notifier = nullptr;

    if (m_display) {
        waywallen_display_disconnect(m_display);
        waywallen_display_destroy(m_display);
        m_display = nullptr;
    }

    m_texturesValid = false;
    m_glTextures.clear();
    m_currentSlot = -1;
    m_textureCount = 0;
    m_pendingRelease = false;
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
    if (!enabled && m_reconnectTimer) {
        m_reconnectTimer->stop();
    }
}

void WaywallenDisplay::setStatus(Status s) {
    if (m_status == s) return;
    m_status = s;
    emit statusChanged();
}

// ---------------------------------------------------------------------------
// Connection + reconnect
// ---------------------------------------------------------------------------

void WaywallenDisplay::componentComplete() {
    QQuickItem::componentComplete();
    if (window()) {
        onWindowReady();
    } else {
        connect(this, &QQuickItem::windowChanged,
                this, &WaywallenDisplay::onWindowReady);
    }
}

void WaywallenDisplay::onWindowReady() {
    if (!window() || m_display) return;
    if (!window()->isSceneGraphInitialized()) {
        connect(window(), &QQuickWindow::sceneGraphInitialized,
                this, &WaywallenDisplay::tryConnect,
                Qt::UniqueConnection);
        return;
    }
    tryConnect();
}

void WaywallenDisplay::tryConnect() {
    if (m_display) return;
    setStatus(Connecting);

    waywallen_display_callbacks_t cb {};
    cb.on_textures_ready = c_on_textures_ready;
    cb.on_textures_releasing = c_on_textures_releasing;
    cb.on_config = c_on_config;
    cb.on_frame_ready = c_on_frame_ready;
    cb.on_disconnected = c_on_disconnected;
    cb.user_data = this;

    m_display = waywallen_display_new(&cb);
    if (!m_display) {
        qWarning("WaywallenDisplay: new() failed");
        setStatus(Error);
        scheduleReconnect();
        return;
    }

    // Bind EGL from Qt's scene graph.
    if (window()) {
        auto *rif = window()->rendererInterface();
        if (rif) {
            void *eglDisplay = rif->getResource(window(), "eglDisplay");
            if (eglDisplay) {
                waywallen_egl_ctx_t egl_ctx {};
                egl_ctx.egl_display = eglDisplay;
                egl_ctx.get_proc_address = nullptr;
                waywallen_display_bind_egl(m_display, &egl_ctx);
            }
        }
    }

    const QByteArray sockPath = m_socketPath.toUtf8();
    const QByteArray name = m_displayName.toUtf8();
    int rc = waywallen_display_connect(
        m_display,
        sockPath.isEmpty() ? nullptr : sockPath.constData(),
        name.constData(),
        static_cast<uint32_t>(m_displayWidth),
        static_cast<uint32_t>(m_displayHeight),
        60000);

    if (rc != WAYWALLEN_OK) {
        qWarning("WaywallenDisplay: connect failed (rc=%d)", rc);
        waywallen_display_destroy(m_display);
        m_display = nullptr;
        setStatus(Disconnected);
        scheduleReconnect();
        return;
    }

    // Success — reset backoff.
    m_reconnectDelay = 1000;
    m_connected = true;
    emit connectedChanged();
    setStatus(Idle);

    int fd = waywallen_display_get_fd(m_display);
    if (fd >= 0) {
        m_notifier = new QSocketNotifier(fd, QSocketNotifier::Read, this);
        connect(m_notifier, &QSocketNotifier::activated,
                this, &WaywallenDisplay::onSocketReadable);
    }
}

void WaywallenDisplay::scheduleReconnect() {
    if (!m_autoReconnect) return;
    if (!m_reconnectTimer) {
        m_reconnectTimer = new QTimer(this);
        m_reconnectTimer->setSingleShot(true);
        connect(m_reconnectTimer, &QTimer::timeout,
                this, &WaywallenDisplay::onReconnectTimer);
    }
    qDebug("WaywallenDisplay: reconnecting in %d ms", m_reconnectDelay);
    m_reconnectTimer->start(m_reconnectDelay);
    // Exponential backoff: 1s → 2s → 4s → ... → 30s max.
    m_reconnectDelay = qMin(m_reconnectDelay * 2, kMaxReconnectDelay);
}

void WaywallenDisplay::onReconnectTimer() {
    if (m_display) return;  // already reconnected
    tryConnect();
}

void WaywallenDisplay::onSocketReadable() {
    if (!m_display) return;
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
    qWarning("WaywallenDisplay: disconnected (err=%d msg=%s)",
             errCode, msg ? msg : "(null)");
    cleanup();
    if (m_connected) {
        m_connected = false;
        emit connectedChanged();
    }
    setStatus(Disconnected);
    update();
    scheduleReconnect();
}

// ---------------------------------------------------------------------------
// Scene graph
// ---------------------------------------------------------------------------

QSGNode *WaywallenDisplay::updatePaintNode(QSGNode *oldNode,
                                           UpdatePaintNodeData *) {
    if (!m_texturesValid || m_currentSlot < 0
        || m_currentSlot >= m_glTextures.size() || !window()) {
        delete oldNode;
        return nullptr;
    }

    auto *node = static_cast<QSGSimpleTextureNode *>(oldNode);
    if (!node) {
        node = new QSGSimpleTextureNode();
        node->setFiltering(QSGTexture::Linear);
        node->setOwnsTexture(true);
    }

    uint glTex = m_glTextures[m_currentSlot];
    QSize texSize(m_texWidth, m_texHeight);

    QSGTexture *sgTex = QNativeInterface::QSGOpenGLTexture::fromNativeExternalOES(
        glTex, window(), texSize,
        QQuickWindow::TextureHasAlphaChannel);

    if (sgTex) {
        node->setTexture(sgTex);
    } else {
        delete node;
        return nullptr;
    }

    node->setRect(boundingRect());
    return node;
}
