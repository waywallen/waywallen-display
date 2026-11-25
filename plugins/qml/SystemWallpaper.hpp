#pragma once

#include <QObject>
#include <QDBusContext>
#include <qqml.h>

class PlasmaBackgroundService : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.waywallen.Display.Background1")
    Q_PROPERTY(bool CanAutoUpdateStaticBackground READ canAutoUpdateStaticBackground CONSTANT)
public:
    explicit PlasmaBackgroundService(QObject* parent = nullptr);
    ~PlasmaBackgroundService() override;
    bool           registered() const { return m_registered; }
    bool           canAutoUpdateStaticBackground() const { return true; }
    static QString script(const QString& path);

public Q_SLOTS:
    void SetStaticBackground(const QString& path);

private:
    bool m_registered = false;
    bool m_busy       = false;
};

// One provider per plasmashell process, shared by all desktop wallpaper instances.
class SystemWallpaper : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)
public:
    explicit SystemWallpaper(QObject* parent = nullptr): QObject(parent) {}
    ~SystemWallpaper() override;
    bool enabled() const { return m_enabled; }
    void setEnabled(bool enabled);

Q_SIGNALS:
    void enabledChanged();

private:
    bool m_enabled = false;
};
