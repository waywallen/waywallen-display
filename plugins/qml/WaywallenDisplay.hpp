#pragma once

#include "PresentationState.hpp"

#include <waywallen_display_protocol_types.h>

#include <QColor>
#include <QMutex>
#include <QPointer>
#include <QQuickItem>
#include <QRectF>
#include <QSocketNotifier>
#include <QString>
#include <QTimer>
#include <QVector>
#include <qqml.h>
#include <cstdint>
#include <memory>

#ifdef WW_HAVE_VULKAN
#    include "backend_vulkan_blit.h"
#endif

struct waywallen_display;
typedef struct waywallen_display waywallen_display_t;
struct waywallen_textures;
typedef struct waywallen_textures waywallen_textures_t;
struct waywallen_binding;
typedef struct waywallen_binding waywallen_binding_t;
struct waywallen_frame;
typedef struct waywallen_frame waywallen_frame_t;
class RenderSessionResources;
class QScreen;

class WaywallenDisplay : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QString socketPath READ socketPath WRITE setSocketPath NOTIFY socketPathChanged)
    Q_PROPERTY(QString displayName READ displayName WRITE setDisplayName NOTIFY displayNameChanged)
    Q_PROPERTY(QString instanceId READ instanceId WRITE setInstanceId NOTIFY instanceIdChanged)
    Q_PROPERTY(int displayWidth READ displayWidth WRITE setDisplayWidth NOTIFY displayWidthChanged)
    Q_PROPERTY(
        int displayHeight READ displayHeight WRITE setDisplayHeight NOTIFY displayHeightChanged)
    Q_PROPERTY(int framesReceived READ framesReceived NOTIFY framesReceivedChanged)
    Q_PROPERTY(qulonglong contentRevision READ contentRevision NOTIFY contentRevisionChanged)
    Q_PROPERTY(qulonglong displayId READ displayId NOTIFY displayIdChanged)
    Q_PROPERTY(ConnState connState READ connState NOTIFY connStateChanged)
    Q_PROPERTY(StreamState streamState READ streamState NOTIFY streamStateChanged)
    Q_PROPERTY(DisconnectReason lastDisconnectReason READ lastDisconnectReason NOTIFY
                   lastDisconnectChanged)
    Q_PROPERTY(
        QString lastDisconnectMessage READ lastDisconnectMessage NOTIFY lastDisconnectChanged)
    // Read-only: the renderer publishes the clear color via the
    // daemon's composition config; consumers do NOT override it.
    Q_PROPERTY(QColor clearColor READ clearColor NOTIFY clearColorChanged)
    Q_PROPERTY(
        bool autoReconnect READ autoReconnect WRITE setAutoReconnect NOTIFY autoReconnectChanged)
    Q_PROPERTY(bool mouseForwardEnabled READ mouseForwardEnabled WRITE setMouseForwardEnabled NOTIFY
                   mouseForwardEnabledChanged)
    // Bitmask of WAYWALLEN_WIN_HAS_* describing the windows that
    // currently cover this display. Pushed to the daemon as a
    // `window_state` request; the daemon owns the autopause policy.
    Q_PROPERTY(quint32 windowStateFlags READ windowStateFlags WRITE setWindowStateFlags NOTIFY
                   windowStateFlagsChanged)
    Q_PROPERTY(quint32 presentationCapabilities READ presentationCapabilities WRITE
                   setPresentationCapabilities NOTIFY presentationCapabilitiesChanged)
    Q_PROPERTY(PauseEffectKind pauseEffectKind READ pauseEffectKind NOTIFY presentationChanged)
    Q_PROPERTY(int blurRadius READ blurRadius NOTIFY presentationChanged)
    Q_PROPERTY(bool pauseEffectActive READ pauseEffectActive NOTIFY presentationChanged)
    Q_PROPERTY(qulonglong presentationConfigGeneration READ presentationConfigGeneration NOTIFY
                   presentationChanged)
    Q_PROPERTY(qulonglong presentationStateGeneration READ presentationStateGeneration NOTIFY
                   presentationChanged)

public:
    enum ConnState
    {
        Disconnected = 0,
        Connecting,
        Handshaking,
        Connected,
        Error,
    };
    Q_ENUM(ConnState)

    enum PauseEffectKind
    {
        NonePauseEffect = WAYWALLEN_PAUSE_EFFECT_KIND_NONE,
        BlurPauseEffect = WAYWALLEN_PAUSE_EFFECT_KIND_BLUR,
    };
    Q_ENUM(PauseEffectKind)

    enum StreamState
    {
        Inactive = 0,
        Active,
    };
    Q_ENUM(StreamState)

    // Numeric values mirror waywallen_disconnect_reason_t in
    // include/waywallen_display.h — keep in sync.
    enum DisconnectReason
    {
        None               = 0,
        VersionUnsupported = 1,
        ProtocolMismatch   = 2,
        DaemonError        = 3,
        HandshakeFailed    = 4,
        SocketIo           = 5,
        ProtocolError      = 6,
        DaemonGone         = 7,
    };
    Q_ENUM(DisconnectReason)

    enum PresentationCapability
    {
        PauseBlurCapability = 1u << 0,
    };
    Q_ENUM(PresentationCapability)

    explicit WaywallenDisplay(QQuickItem* parent = nullptr);
    ~WaywallenDisplay() override;

    QString socketPath() const { return m_socketPath; }
    void    setSocketPath(const QString& path);

    QString displayName() const { return m_displayName; }
    void    setDisplayName(const QString& name);

    QString instanceId() const { return effectiveInstanceId(); }
    void    setInstanceId(const QString& id);

    int  displayWidth() const { return m_displayWidth; }
    void setDisplayWidth(int w);

    int  displayHeight() const { return m_displayHeight; }
    void setDisplayHeight(int h);

    int        framesReceived() const { return m_framesReceived; }
    qulonglong contentRevision() const { return m_contentRevision; }

    qulonglong displayId() const { return m_displayId; }

    ConnState   connState() const { return m_connState; }
    StreamState streamState() const { return m_streamState; }

    DisconnectReason lastDisconnectReason() const { return m_lastReason; }
    QString          lastDisconnectMessage() const { return m_lastMessage; }

    QColor clearColor() const { return m_clearColor; }

    bool autoReconnect() const { return m_autoReconnect; }
    void setAutoReconnect(bool enabled);

    bool mouseForwardEnabled() const { return m_mouseForwardEnabled; }
    void setMouseForwardEnabled(bool enabled);

    quint32 windowStateFlags() const { return m_windowStateFlags; }
    void    setWindowStateFlags(quint32 flags);

    quint32 presentationCapabilities() const { return m_presentationCapabilities; }
    void    setPresentationCapabilities(quint32 capabilities);

    PauseEffectKind pauseEffectKind() const { return m_pauseEffectKind; }
    int             blurRadius() const { return m_blurRadius; }
    bool            pauseEffectActive() const { return m_pauseEffectActive; }
    qulonglong      presentationConfigGeneration() const { return m_presentationConfigGeneration; }
    qulonglong      presentationStateGeneration() const { return m_presentationStateGeneration; }

    // Attempt to connect now. No-op when already Connected. Triggered
    // automatically by the DBus NameOwnerChanged / Daemon Ready signals
    // (see setupDBusWatcher) and by the internal backoff timer (see
    // scheduleReconnectBackoff); also exposed for tests and manual cues.
    Q_INVOKABLE void requestReconnect();

    bool eventFilter(QObject* obj, QEvent* event) override;

signals:
    void socketPathChanged();
    void displayNameChanged();
    void instanceIdChanged();
    void displayWidthChanged();
    void displayHeightChanged();
    void framesReceivedChanged();
    void contentRevisionChanged();
    void displayIdChanged();
    void connStateChanged();
    void streamStateChanged();
    void lastDisconnectChanged();
    void clearColorChanged();
    void autoReconnectChanged();
    void mouseForwardEnabledChanged();
    void windowStateFlagsChanged();
    void presentationCapabilitiesChanged();
    void presentationChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData* data) override;
    void     componentComplete() override;
    void     releaseResources() override;

private slots:
    void onSocketReadable();
    void onSocketWritable();
    void onHandshakeIO();
    void onWindowReady();
    void onScreenChanged(QScreen* screen);
    void onDaemonNameOwnerChanged(const QString& name, const QString& oldOwner,
                                  const QString& newOwner);
    void onDaemonReadySignal();
    void onReconnectTimer();
    void pushSizeUpdate();

private:
    using ConfigSnapshot  = PresentationState::Config;
    using ContentSnapshot = PresentationState::Content;

    void                                    tryConnect();
    void                                    cleanup();
    std::shared_ptr<RenderSessionResources> takeRenderSessionResources();
    std::shared_ptr<RenderSessionResources> renderSessionResources() const;
    waywallen_display_t*                    displayHandle() const;
    void                                    setupDBusWatcher();
    void                                    flushPendingRelease();
    void                                    handleDisconnect(int errCode, const char* msg);
    // Backoff fallback for missed NameOwnerChanged / Ready signals
    // (e.g. daemon already up before setupDBusWatcher subscribed).
    void     scheduleReconnectBackoff();
    void     applyPresentationSnapshot(const waywallen_presentation_snapshot_t& presentation);
    void     applyPresentationState(const waywallen_presentation_state_t& state);
    void     resetPresentation();
    void     setConnState(ConnState s);
    void     setStreamState(StreamState s);
    QString  screenIdentityKey() const;
    QString  effectiveInstanceId() const;
    uint32_t screenRefreshMhz() const;
    void     reportFrameArmed(uint64_t generation, uint64_t seq);
    void     signalFrameRelease(int fd, uint64_t generation, uint64_t seq, const char* context);
    /* Probe wants_writable and toggle m_notifierWrite::setEnabled.
     * Call after any post-handshake send that may have left bytes
     * queued in the lib's outbox (update_size, pointer events). */
    void armWriteNotifier();

    bool bindEglBackend();
    bool bindVulkanBackend();
    void ensureGlTextures();
    enum class EglBlitResult
    {
        Failed,
        FailedAfterGpuWork,
        CurrentUpdated,
        CandidateReady,
    };
    /* Blit an imported GL texture into the current shadow or a separate
     * replacement. Render thread only. */
    EglBlitResult blitEglShadow(int slot, int width, int height, bool forceReplace);
    void          releaseEglFrame(int releaseSyncobjFd, bool afterGpuWork, uint64_t generation,
                                  uint64_t seq);
    /* Render-thread job: drains m_pendingEgl, ensures GL textures,
     * runs blitEglShadow. Scheduled from c_on_frame_ready via
     * scheduleRenderJob(BeforeSynchronizingStage). */
    void renderThreadBlitEgl();
    void commitPresentedContent(uint64_t generation, int width, int height, uint32_t fourcc,
                                const ConfigSnapshot& config);
    void publishPresentationCommit(qulonglong serial, const QColor& clearColor);
    void setPresentedClearColor(const QColor& color);

    // C callback trampolines.
    static void c_on_binding_ready(void* ud, const waywallen_binding_t* binding);
    static void c_on_textures_releasing(void* ud, const waywallen_textures_t* t);
    static void c_on_composition_config(void* ud, const waywallen_composition_config_t* config);
    static void c_on_frame_ready(void* ud, const waywallen_frame_t* f);
    static void c_on_presentation_snapshot(void*                                    ud,
                                           const waywallen_presentation_snapshot_t* presentation);
    static void c_on_presentation_state(void* ud, const waywallen_presentation_state_t* state);
    static void c_on_disconnected(void* ud, int err, const char* msg);

    // Properties.
    QString          m_socketPath;
    QString          m_displayName { QStringLiteral("qml-display") };
    QString          m_instanceId;
    int              m_displayWidth { 1920 };
    int              m_displayHeight { 1080 };
    int              m_framesReceived { 0 };
    qulonglong       m_contentRevision { 0 };
    qulonglong       m_displayId { 0 };
    ConnState        m_connState { Disconnected };
    StreamState      m_streamState { Inactive };
    DisconnectReason m_lastReason { None };
    QString          m_lastMessage;
    QColor           m_clearColor { Qt::black };
    bool             m_autoReconnect { true };
    bool             m_mouseForwardEnabled { true };
    bool             m_filterInstalled { false };
    // Latest QML-supplied flags. Echoed to the daemon when the
    // connection is up; held until then so the post-handshake state
    // matches whatever the WindowModel last computed (replayed from
    // setConnState(Connected)).
    quint32         m_windowStateFlags { 0 };
    bool            m_windowStateFlagsDirty { false };
    quint32         m_presentationCapabilities { 0 };
    PauseEffectKind m_pauseEffectKind { NonePauseEffect };
    int             m_blurRadius { 30 };
    bool            m_pauseEffectActive { false };
    qulonglong      m_presentationConfigGeneration { 0 };
    qulonglong      m_presentationStateGeneration { 0 };

    mutable QMutex                          m_resourcesMutex;
    std::shared_ptr<RenderSessionResources> m_renderResources;
    /* QPointer so cross-thread access (sceneGraphInvalidated lambda
     * runs on render thread) is safe — QPointer auto-clears when the
     * underlying QObject is destroyed via deleteLater, and reads of
     * a destroyed QPointer are well-defined (return nullptr). Raw
     * pointers here used to require careful disconnect+deleteLater+
     * null-write ordering across threads. */
    QPointer<QSocketNotifier> m_notifier;
    QPointer<QSocketNotifier> m_notifierWrite;

    // Coalesces display mode changes into one metrics snapshot.
    QTimer   m_updateSizeTimer;
    int      m_lastPushedWidth { -1 };
    int      m_lastPushedHeight { -1 };
    uint32_t m_lastPushedRefreshMhz { 0 };

    // Backoff fallback for reconnect (see scheduleReconnectBackoff).
    QTimer m_reconnectTimer;
    int    m_reconnectDelayMs { 2000 };

    // Backend detected from Qt's scene graph.
    enum ActiveBackend
    {
        BackendNone,
        BackendEGL,
        BackendVulkan
    };
    ActiveBackend m_activeBackend { BackendNone };

    PresentationState m_presentationState;
    qulonglong        m_presentationSerial { 0 };
    qulonglong        m_notifiedPresentationSerial { 0 };

    // EGL texture state (GL textures created lazily on render thread).
    bool          m_eglImagesValid { false };
    bool          m_glTexturesCreated { false };
    QVector<uint> m_glTextures;
    uint32_t      m_textureCount { 0 };

    // Host-owned shadow GL texture for the EGL path. Mirrors the
    // Vulkan blitter's shadow image: each frame we blit imported
    // → shadow, and Qt samples the shadow. Decouples the visible
    // texture from pool transitions so that on `unbind` the prior
    // frame stays on screen until the next pool's first frame
    // arrives — same continuity the Vulkan path gets for free.
    // Most-recent unblitted EGL frame, populated on the GUI thread by
    // c_on_frame_ready and consumed on the render thread by
    // renderThreadBlitEgl. Mirrors PendingVkFrame but EGL has no
    // acquire semaphore (GL is queue-ordered, no cross-process sync
    // needed). Mutex-protected by m_pendingMutex.
    struct PendingEglFrame {
        bool     valid { false };
        int      slot { -1 };
        int      releaseSyncobjFd { -1 };
        uint64_t bufferGeneration { 0 };
        uint64_t seq { 0 };
    };
    PendingEglFrame m_pendingEgl;
    ContentSnapshot m_preparedEglContent;

    // Shared between EGL m_pendingEgl and Vulkan m_pendingVk.
    QMutex m_pendingMutex;

    // Vulkan texture state.
    bool           m_vkImagesValid { false };
    QVector<void*> m_vkImages;

#ifdef WW_HAVE_VULKAN
    // Cached on bindVulkanBackend.
    void*    m_vkInstance { nullptr };
    void*    m_vkPhys { nullptr };
    void*    m_vkDevice { nullptr };
    void*    m_vkQueue { nullptr };
    uint32_t m_vkQfi { 0 };
    void* (*m_vkGipa)(void*, const char*) { nullptr };

    // Most-recent unblitted frame, populated on the main thread by
    // c_on_frame_ready and consumed on the render thread by
    // updatePaintNode. Older pending frames are released immediately
    // because they were never submitted to GPU work.
    struct PendingVkFrame {
        bool     valid { false };
        int      slot { -1 };
        void*    acquireSem { nullptr }; // VkSemaphore (lib-imported sync_fd)
        int      releaseSyncobjFd { -1 };
        uint64_t bufferGeneration { 0 };
        uint64_t seq { 0 };
    };
    PendingVkFrame m_pendingVk;
#endif
};
