#pragma once

#include <QObject>
#include <QString>
#include <qqml.h>

#include <waywallen_display.h>

// Lightweight info object surfacing libwaywallen_display build metadata to
// QML. Instantiated by ImportTest{,Embed}.qml so config.qml can probe the
// module and read the version off the created instance in one go.
class PluginInfo : public QObject {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(int versionMajor READ versionMajor CONSTANT)
    Q_PROPERTY(int versionMinor READ versionMinor CONSTANT)
    Q_PROPERTY(int versionPatch READ versionPatch CONSTANT)
    Q_PROPERTY(QString version READ version CONSTANT)

public:
    explicit PluginInfo(QObject *parent = nullptr) : QObject(parent) {
        m_v = waywallen_display_version();
    }

    int versionMajor() const { return static_cast<int>(m_v.major); }
    int versionMinor() const { return static_cast<int>(m_v.minor); }
    int versionPatch() const { return static_cast<int>(m_v.patch); }

    QString version() const {
        return QStringLiteral("%1.%2.%3")
            .arg(m_v.major).arg(m_v.minor).arg(m_v.patch);
    }

private:
    waywallen_display_version_t m_v {};
};
