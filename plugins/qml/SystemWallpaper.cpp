#include "SystemWallpaper.hpp"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUrl>

namespace
{
constexpr auto           busName    = "org.waywallen.Display.Background";
constexpr auto           objectPath = "/org/waywallen/Display/Background";
PlasmaBackgroundService* service    = nullptr;
int                      users      = 0;
} // namespace

PlasmaBackgroundService::PlasmaBackgroundService(QObject* parent): QObject(parent) {
    auto bus = QDBusConnection::sessionBus();
    if (! bus.registerObject(
            QString::fromLatin1(objectPath), this,
            QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllProperties)) {
        qWarning("Waywallen: could not export system background provider");
        return;
    }
    m_registered = bus.registerService(QString::fromLatin1(busName));
    if (! m_registered) {
        bus.unregisterObject(QString::fromLatin1(objectPath));
        qWarning("Waywallen: could not own system background bus name");
    }
}

PlasmaBackgroundService::~PlasmaBackgroundService() {
    if (m_registered) {
        auto bus = QDBusConnection::sessionBus();
        bus.unregisterService(QString::fromLatin1(busName));
        bus.unregisterObject(QString::fromLatin1(objectPath));
    }
}

QString PlasmaBackgroundService::script(const QString& path) {
    const auto encoded =
        QJsonDocument(QJsonArray { QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded) })
            .toJson(QJsonDocument::Compact);
    const auto image = QString::fromUtf8(encoded.mid(1, encoded.size() - 2));
    // Writing the image plugin's config must not select it over the live wallpaper plugin.
    return QStringLiteral(
               "var ds = desktopsForActivity(currentActivity()).filter(function(d) { "
               "return d.wallpaperPlugin === 'org.waywallen.kde'; }); "
               "if (!ds.length) throw new Error('No active Waywallen desktops'); "
               "for (var i = 0; i < ds.length; ++i) { var d = ds[i]; "
               "d.currentConfigGroup = ['Wallpaper', 'org.kde.image', 'General']; "
               "d.writeConfig('Image', %1); }")
        .arg(image);
}

void PlasmaBackgroundService::SetStaticBackground(const QString& path) {
    if (! calledFromDBus()) return;
    const QFileInfo info(path);
    if (path.size() > 4096 || path.contains(QChar::Null) || ! info.isAbsolute() ||
        ! info.isFile() || ! QImageReader(path).canRead()) {
        sendErrorReply(QDBusError::InvalidArgs,
                       QStringLiteral("Expected an absolute path to a readable image"));
        return;
    }
    if (m_busy) {
        sendErrorReply(QDBusError::Failed,
                       QStringLiteral("A system background update is already in progress"));
        return;
    }
    m_busy = true;
    setDelayedReply(true);
    const auto replyTo = message();
    const auto bus     = connection();
    auto       request = QDBusMessage::createMethodCall(QStringLiteral("org.kde.plasmashell"),
                                                  QStringLiteral("/PlasmaShell"),
                                                  QStringLiteral("org.kde.PlasmaShell"),
                                                  QStringLiteral("evaluateScript"));
    request << script(path);
    auto* watcher = new QDBusPendingCallWatcher(bus.asyncCall(request, 10000), this);
    connect(watcher,
            &QDBusPendingCallWatcher::finished,
            this,
            [this, bus, replyTo](QDBusPendingCallWatcher* call) {
                const QDBusPendingReply<QString> result = *call;
                bus.send(result.isError() ? replyTo.createErrorReply(result.error())
                                          : replyTo.createReply());
                m_busy = false;
                call->deleteLater();
            });
}

SystemWallpaper::~SystemWallpaper() { setEnabled(false); }

void SystemWallpaper::setEnabled(bool enabled) {
    if (m_enabled == enabled) return;
    m_enabled = enabled;
    if (enabled) {
        if (++users == 1) service = new PlasmaBackgroundService(QCoreApplication::instance());
    } else if (--users == 0) {
        delete service;
        service = nullptr;
    }
    Q_EMIT enabledChanged();
}
