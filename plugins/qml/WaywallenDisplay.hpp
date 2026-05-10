#pragma once

#include <QColor>
#include <QMutex>
#include <QQuickItem>
#include <QRectF>
#include <QSocketNotifier>
#include <QString>
#include <QTimer>
#include <QVector>
#include <qqml.h>

#ifdef WW_HAVE_VULKAN
#include "backend_vulkan_blit.h"
#endif

struct waywallen_display;
typedef struct waywallen_display waywallen_display_t;
struct waywallen_textures;
typedef struct waywallen_textures waywallen_textures_t;
struct waywallen_config;
typedef struct waywallen_config waywallen_config_t;
struct waywallen_frame;
typedef struct waywallen_frame waywallen_frame_t;

class WaywallenDisplay : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QString socketPath READ socketPath WRITE setSocketPath
                   NOTIFY socketPathChanged)
    Q_PROPERTY(QString displayName READ displayName WRITE setDisplayName
                   NOTIFY displayNameChanged)
    Q_PROPERTY(QString instanceId READ instanceId WRITE setInstanceId
                   NOTIFY instanceIdChanged)
    Q_PROPERTY(int displayWidth READ displayWidth WRITE setDisplayWidth
                   NOTIFY displayWidthChanged)
    Q_PROPERTY(int displayHeight READ displayHeight WRITE setDisplayHeight
                   NOTIFY displayHeightChanged)
    Q_PROPERTY(int framesReceived READ framesReceived
                   NOTIFY framesReceivedChanged)
    Q_PROPERTY(qulonglong displayId READ displayId NOTIFY displayIdChanged)
    Q_PROPERTY(ConnState connState READ connState NOTIFY connStateChanged)
    Q_PROPERTY(StreamState streamState READ streamState
                   NOTIFY streamStateChanged)
    Q_PROPERTY(DisconnectReason lastDisconnectReason READ lastDisconnectReason
                   NOTIFY lastDisconnectChanged)
    Q_PROPERTY(QString lastDisconnectMessage READ lastDisconnectMessage
                   NOTIFY lastDisconnectChanged)
    // Read-only: the renderer publishes the clear color via the
    // daemon's `set_config` event; consumers do NOT override it.
    Q_PROPERTY(QColor clearColor READ clearColor NOTIFY clearColorChanged)
    Q_PROPERTY(bool autoReconnect READ autoReconnect WRITE setAutoReconnect
                   NOTIFY autoReconnectChanged)
    Q_PROPERTY(bool mouseForwardEnabled READ mouseForwardEnabled
                   WRITE setMouseForwardEnabled
                   NOTIFY mouseForwardEnabledChanged)

public:
    enum ConnState {
        Disconnected = 0,
        Connecting,
        Handshaking,
        Connected,
        Error,
    };
    Q_ENUM(ConnState)

    enum StreamState {
        Inactive = 0,
        Active,
    };
    Q_ENUM(StreamState)

    // Numeric values mirror waywallen_disconnect_reason_t in
    // include/waywallen_display.h — keep in sync.
    enum DisconnectReason {
        None                = 0,
        VersionUnsupported  = 1,
        ProtocolMismatch    = 2,
        DaemonError         = 3,
        HandshakeFailed     = 4,
        SocketIo            = 5,
        ProtocolError       = 6,
        DaemonGone          = 7,
    };
    Q_ENUM(DisconnectReason)

    explicit WaywallenDisplay(QQuickItem *parent = nullptr);
    ~WaywallenDisplay() override;

    QString socketPath() const { return m_socketPath; }
    void setSocketPath(const QString &path);

    QString displayName() const { return m_displayName; }
    void setDisplayName(const QString &name);

    QString instanceId() const { return m_instanceId; }
    void setInstanceId(const QString &id);

    int displayWidth() const { return m_displayWidth; }
    void setDisplayWidth(int w);

    int displayHeight() const { return m_displayHeight; }
    void setDisplayHeight(int h);

    int framesReceived() const { return m_framesReceived; }

    qulonglong displayId() const { return m_displayId; }

    ConnState connState() const { return m_connState; }
    StreamState streamState() const { return m_streamState; }

    DisconnectReason lastDisconnectReason() const { return m_lastReason; }
    QString lastDisconnectMessage() const { return m_lastMessage; }

    QColor clearColor() const { return m_clearColor; }

    bool autoReconnect() const { return m_autoReconnect; }
    void setAutoReconnect(bool enabled);

    bool mouseForwardEnabled() const { return m_mouseForwardEnabled; }
    void setMouseForwardEnabled(bool enabled);

    // Attempt to connect now. No-op when already Connected. Triggered
    // automatically by the DBus NameOwnerChanged / Daemon Ready signals
    // (see setupDBusWatcher); also exposed for tests and manual cues.
    // There is no internal retry timer — recovery relies entirely on
    // DBus signals fired when the daemon re-appears.
    Q_INVOKABLE void requestReconnect();

    bool eventFilter(QObject *obj, QEvent *event) override;

signals:
    void socketPathChanged();
    void displayNameChanged();
    void instanceIdChanged();
    void displayWidthChanged();
    void displayHeightChanged();
    void framesReceivedChanged();
    void displayIdChanged();
    void connStateChanged();
    void streamStateChanged();
    void lastDisconnectChanged();
    void clearColorChanged();
    void autoReconnectChanged();
    void mouseForwardEnabledChanged();

protected:
    QSGNode *updatePaintNode(QSGNode *oldNode,
                             UpdatePaintNodeData *data) override;
    void componentComplete() override;

private slots:
    void onSocketReadable();
    void onHandshakeIO();
    void onWindowReady();
    void onDaemonNameOwnerChanged(const QString &name,
                                  const QString &oldOwner,
                                  const QString &newOwner);
    void onDaemonReadySignal();
    void pushSizeUpdate();

private:
    void tryConnect();
    void cleanup();
    void setupDBusWatcher();
    void flushPendingRelease();
    void handleDisconnect(int errCode, const char *msg);
    void setConnState(ConnState s);
    void setStreamState(StreamState s);

    bool bindEglBackend();
    bool bindVulkanBackend();
    void ensureGlTextures();

    // C callback trampolines.
    static void c_on_textures_ready(void *ud, const waywallen_textures_t *t);
    static void c_on_textures_releasing(void *ud, const waywallen_textures_t *t);
    static void c_on_config(void *ud, const waywallen_config_t *c);
    static void c_on_frame_ready(void *ud, const waywallen_frame_t *f);
    static void c_on_disconnected(void *ud, int err, const char *msg);

    // Properties.
    QString m_socketPath;
    QString m_displayName { QStringLiteral("qml-display") };
    QString m_instanceId;
    int m_displayWidth { 1920 };
    int m_displayHeight { 1080 };
    int m_framesReceived { 0 };
    qulonglong m_displayId { 0 };
    ConnState m_connState { Disconnected };
    StreamState m_streamState { Inactive };
    DisconnectReason m_lastReason { None };
    QString m_lastMessage;
    QColor m_clearColor { Qt::black };
    bool m_autoReconnect { true };
    bool m_mouseForwardEnabled { true };
    bool m_filterInstalled { false };

    // C library handle.
    waywallen_display_t *m_display { nullptr };
    QSocketNotifier *m_notifier { nullptr };
    QSocketNotifier *m_notifierWrite { nullptr };

    // Coalesces a flood of width/height changes during a window resize
    // into a single `update_display` wire message. Last reported size is
    // kept so we don't push duplicate updates.
    QTimer m_updateSizeTimer;
    int m_lastPushedWidth { -1 };
    int m_lastPushedHeight { -1 };

    // Backend detected from Qt's scene graph.
    enum ActiveBackend { BackendNone, BackendEGL, BackendVulkan };
    ActiveBackend m_activeBackend { BackendNone };

    // EGL texture state (GL textures created lazily on render thread).
    bool m_eglImagesValid { false };
    bool m_glTexturesCreated { false };
    QVector<uint> m_glTextures;
    int m_texWidth { 0 };
    int m_texHeight { 0 };
    uint32_t m_textureCount { 0 };
    int m_currentSlot { -1 };

    // Vulkan texture state.
    bool m_vkImagesValid { false };
    QVector<void *> m_vkImages;
    uint32_t m_vkFourcc { 0 };

#ifdef WW_HAVE_VULKAN
    // Owned by render thread; created on first updatePaintNode after
    // a Vulkan textures_ready. Copies imported dmabuf images into a
    // sampler-friendly OPTIMAL VkImage Qt actually samples.
    ww_vk_blitter_t m_vkBlitter {};
    bool m_vkBlitterInited { false };

    // Cached on bindVulkanBackend.
    void *m_vkInstance { nullptr };
    void *m_vkPhys     { nullptr };
    void *m_vkDevice   { nullptr };
    void *m_vkQueue    { nullptr };
    uint32_t m_vkQfi   { 0 };
    void *(*m_vkGipa)(void *, const char *) { nullptr };

    // Most-recent unblitted frame, populated on the main thread by
    // c_on_frame_ready and consumed on the render thread by
    // updatePaintNode. Older pending frame is dropped (its
    // release_syncobj_fd is closed; daemon times out the slot).
    struct PendingVkFrame {
        bool valid { false };
        int slot { -1 };
        void *acquireSem { nullptr }; // VkSemaphore (lib-imported sync_fd)
        int releaseSyncobjFd { -1 };
    };
    QMutex m_pendingMutex;
    PendingVkFrame m_pendingVk;
#endif

    // Config from on_config.
    QRectF m_sourceRect;
    QRectF m_destRect;

    // Frame release tracking.
    bool m_pendingRelease { false };
    uint32_t m_pendingReleaseIdx { 0 };
    uint64_t m_pendingReleaseSeq { 0 };

    // EGL path: per-frame release_syncobj fd handed to us via
    // on_frame_ready. We signal it on the *next* frame_ready (in
    // flushPendingRelease) so the daemon's reaper sees a real signal
    // instead of timing out and force-signaling. -1 when no fd is held.
    // The Vulkan path uses the separate m_pendingVk.releaseSyncobjFd
    // and signals from the blitter after the GPU copy completes.
    int m_pendingEglReleaseSyncobjFd { -1 };
};
