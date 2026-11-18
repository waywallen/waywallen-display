#include "../plugins/qml/SystemWallpaper.hpp"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>
#include <QEventLoop>
#include <QFile>
#include <QJSEngine>
#include <QTemporaryDir>
#include <QTimer>

#define CHECK(condition) \
    do { \
        if (!(condition)) qFatal("%s:%d: %s", __FILE__, __LINE__, #condition); \
    } while (false)

class FakePlasma : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.PlasmaShell")
public:
    QString received;
    bool    fail = false;
public Q_SLOTS:
    QString evaluateScript(const QString& script) {
        received = script;
        if (fail) sendErrorReply(QDBusError::Failed, QStringLiteral("test desktop failure"));
        return {};
    }
};

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    auto             server = QDBusConnection::sessionBus();
    CHECK(server.isConnected());
    FakePlasma plasma;
    const bool registered = server.registerService(QStringLiteral("org.kde.plasmashell"));
    CHECK(registered);
    const bool exported = server.registerObject(
        QStringLiteral("/PlasmaShell"), &plasma, QDBusConnection::ExportAllSlots);
    CHECK(exported);
    QTemporaryDir directory;
    const QString image = directory.filePath(QStringLiteral("a'\"b.ppm"));
    QFile         file(image);
    const bool opened = file.open(QIODevice::WriteOnly);
    CHECK(opened);
    file.write("P6\n1 1\n255\n\xff\0\0", 14);
    file.close();
    const auto client = QDBusConnection::connectToBus(QDBusConnection::SessionBus,
                                                      QStringLiteral("background-test-client"));
    auto requestCall = [&client](const QDBusMessage& request) {
        QDBusPendingCallWatcher watcher(client.asyncCall(request, 2000));
        QEventLoop              loop;
        QObject::connect(&watcher, &QDBusPendingCallWatcher::finished, &loop, &QEventLoop::quit);
        QTimer::singleShot(3000, &loop, &QEventLoop::quit);
        if (! watcher.isFinished()) loop.exec();
        CHECK(watcher.isFinished());
        return watcher.reply();
    };
    auto call = [&requestCall](const QString& path) {
        auto request =
            QDBusMessage::createMethodCall(QStringLiteral("org.waywallen.Display.Background"),
                                           QStringLiteral("/org/waywallen/Display/Background"),
                                           QStringLiteral("org.waywallen.Display.Background1"),
                                           QStringLiteral("SetStaticBackground"));
        request << path;
        return QDBusPendingReply<>(requestCall(request)).error();
    };
    {
        SystemWallpaper first;
        SystemWallpaper second;
        first.setEnabled(true);
        second.setEnabled(true);
        auto request = QDBusMessage::createMethodCall(
            QStringLiteral("org.waywallen.Display.Background"),
            QStringLiteral("/org/waywallen/Display/Background"),
            QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Get"));
        request << QStringLiteral("org.waywallen.Display.Background1")
                << QStringLiteral("CanAutoUpdateStaticBackground");
        const QDBusPendingReply<QDBusVariant> capability(requestCall(request));
        CHECK(!capability.isError() && capability.value().variant().toBool());
        auto error = call(image);
        CHECK(!error.isValid());
        CHECK(plasma.received == PlasmaBackgroundService::script(image));
        QJSEngine js;
        const auto setup = js.evaluate(QStringLiteral(R"JS(
            function desktop(plugin) {
                return { wallpaperPlugin: plugin, writes: 0, writeConfig(key, image) {
                    if (key !== 'Image' || !image.includes('%22') ||
                        this.currentConfigGroup.join('/') !== 'Wallpaper/org.kde.image/General')
                        throw new Error('Unexpected image config');
                    this.writes++;
                } };
            }
            var live = desktop('org.waywallen.kde');
            var image = desktop('org.kde.image');
            var inactive = desktop('org.waywallen.kde');
            function desktops() { return [live, image, inactive]; }
            function currentActivity() { return 'current'; }
            function desktopsForActivity(id) { return id === 'current' ? [live, image] : [inactive]; }
        )JS"));
        CHECK(!setup.isError());
        const auto evaluated = js.evaluate(plasma.received);
        CHECK(!evaluated.isError());
        CHECK(js.evaluate(QStringLiteral("live.writes === 1 && image.writes === 0 && inactive.writes === 0 && "
                                        "live.wallpaperPlugin === 'org.waywallen.kde'")).toBool());
        error = call(QStringLiteral("relative.ppm"));
        CHECK(error.type() == QDBusError::InvalidArgs);
        error = call(directory.path());
        CHECK(error.type() == QDBusError::InvalidArgs);
        first.setEnabled(false);
        error = call(image);
        CHECK(!error.isValid());
        plasma.fail = true;
        error = call(image);
        CHECK(error.message().contains(QStringLiteral("test desktop failure")));
    }
    const auto error = call(image);
    CHECK(error.isValid());
    return 0;
}

#include "test_system_wallpaper.moc"
