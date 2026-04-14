#pragma once

#include <QColor>
#include <QQuickItem>
#include <QRectF>
#include <QSocketNotifier>
#include <QString>
#include <QTimer>
#include <QVector>
#include <qqml.h>

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
    Q_PROPERTY(int displayWidth READ displayWidth WRITE setDisplayWidth
                   NOTIFY displayWidthChanged)
    Q_PROPERTY(int displayHeight READ displayHeight WRITE setDisplayHeight
                   NOTIFY displayHeightChanged)
    Q_PROPERTY(int framesReceived READ framesReceived
                   NOTIFY framesReceivedChanged)
    Q_PROPERTY(ConnState connState READ connState NOTIFY connStateChanged)
    Q_PROPERTY(StreamState streamState READ streamState
                   NOTIFY streamStateChanged)
    Q_PROPERTY(QColor clearColor READ clearColor WRITE setClearColor
                   NOTIFY clearColorChanged)
    Q_PROPERTY(bool autoReconnect READ autoReconnect WRITE setAutoReconnect
                   NOTIFY autoReconnectChanged)

public:
    enum ConnState {
        Disconnected = 0,
        Connecting,
        Connected,
        Error,
    };
    Q_ENUM(ConnState)

    enum StreamState {
        Inactive = 0,
        Active,
    };
    Q_ENUM(StreamState)

    explicit WaywallenDisplay(QQuickItem *parent = nullptr);
    ~WaywallenDisplay() override;

    QString socketPath() const { return m_socketPath; }
    void setSocketPath(const QString &path);

    QString displayName() const { return m_displayName; }
    void setDisplayName(const QString &name);

    int displayWidth() const { return m_displayWidth; }
    void setDisplayWidth(int w);

    int displayHeight() const { return m_displayHeight; }
    void setDisplayHeight(int h);

    int framesReceived() const { return m_framesReceived; }

    ConnState connState() const { return m_connState; }
    StreamState streamState() const { return m_streamState; }

    QColor clearColor() const { return m_clearColor; }
    void setClearColor(const QColor &color);

    bool autoReconnect() const { return m_autoReconnect; }
    void setAutoReconnect(bool enabled);

signals:
    void socketPathChanged();
    void displayNameChanged();
    void displayWidthChanged();
    void displayHeightChanged();
    void framesReceivedChanged();
    void connStateChanged();
    void streamStateChanged();
    void clearColorChanged();
    void autoReconnectChanged();

protected:
    QSGNode *updatePaintNode(QSGNode *oldNode,
                             UpdatePaintNodeData *data) override;
    void componentComplete() override;

private slots:
    void onSocketReadable();
    void onWindowReady();
    void onReconnectTimer();

private:
    void tryConnect();
    void cleanup();
    void flushPendingRelease();
    void handleDisconnect(int errCode, const char *msg);
    void setConnState(ConnState s);
    void setStreamState(StreamState s);
    void scheduleReconnect();

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
    int m_displayWidth { 1920 };
    int m_displayHeight { 1080 };
    int m_framesReceived { 0 };
    ConnState m_connState { Disconnected };
    StreamState m_streamState { Inactive };
    QColor m_clearColor { Qt::black };
    bool m_autoReconnect { true };

    // C library handle.
    waywallen_display_t *m_display { nullptr };
    QSocketNotifier *m_notifier { nullptr };

    // Reconnect.
    QTimer *m_reconnectTimer { nullptr };
    int m_reconnectDelay { 1000 };  // ms, exponential backoff
    static constexpr int kMaxReconnectDelay = 30000;

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

    // Config from on_config.
    QRectF m_sourceRect;
    QRectF m_destRect;

    // Frame release tracking.
    bool m_pendingRelease { false };
    uint32_t m_pendingReleaseIdx { 0 };
    uint64_t m_pendingReleaseSeq { 0 };
};
