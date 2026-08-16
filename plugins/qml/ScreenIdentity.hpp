#pragma once

#include <QByteArray>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QString>

struct KdeScreenIdentity {
    enum class Source
    {
        None,
        Serial,
        Edid,
        Connector,
    };

    QString id;
    Source  source { Source::None };

    QString sourceName() const {
        switch (source) {
        case Source::Serial: return QStringLiteral("serial");
        case Source::Edid: return QStringLiteral("edid");
        case Source::Connector: return QStringLiteral("connector");
        case Source::None: return {};
        }
        return {};
    }
};

inline bool kdeEdidLooksValid(const QByteArray& bytes) {
    if (bytes.size() < 128 || bytes.size() % 128 != 0) return false;

    static constexpr quint8 kHeader[] = { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 };
    for (int i = 0; i < 8; ++i) {
        if (static_cast<quint8>(bytes[i]) != kHeader[i]) return false;
    }

    for (qsizetype offset = 0; offset < bytes.size(); offset += 128) {
        quint8 sum = 0;
        for (qsizetype i = 0; i < 128; ++i) sum += static_cast<quint8>(bytes[offset + i]);
        if (sum != 0) return false;
    }
    return true;
}

inline bool kdeDrmConnectorDirUsable(const QString& dir) {
    QFile statusFile(dir + QStringLiteral("/status"));
    if (! statusFile.open(QIODevice::ReadOnly)) return true;
    return QString::fromUtf8(statusFile.readAll()).trimmed() == QLatin1String("connected");
}

inline QByteArray kdeReadValidEdidFile(const QString& dir) {
    QFile edidFile(dir + QStringLiteral("/edid"));
    if (! edidFile.open(QIODevice::ReadOnly)) return {};
    const auto bytes = edidFile.readAll();
    if (! kdeEdidLooksValid(bytes)) return {};
    return bytes;
}

inline bool kdeIsCardConnectorDir(const QString& entry, const QString& connector) {
    if (! entry.startsWith(QLatin1String("card"))) return false;
    int i = 4;
    if (i >= entry.size() || ! entry.at(i).isDigit()) return false;
    while (i < entry.size() && entry.at(i).isDigit()) ++i;
    if (i >= entry.size() || entry.at(i) != QLatin1Char('-')) return false;
    return entry.mid(i + 1) == connector;
}

inline QByteArray kdeConnectedEdid(const QString& connector,
                                   const QString& drmRoot = QStringLiteral("/sys/class/drm")) {
    const auto name = connector.trimmed();
    if (name.isEmpty()) return {};

    QDir drm(drmRoot);
    if (! drm.exists()) return {};

    const QString exact = drm.filePath(name);
    if (kdeDrmConnectorDirUsable(exact)) {
        const auto bytes = kdeReadValidEdidFile(exact);
        if (! bytes.isEmpty()) return bytes;
    }

    QStringList matches;
    const auto  entries = drm.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const auto& entry : entries) {
        if (! kdeIsCardConnectorDir(entry, name)) continue;
        matches << drm.filePath(entry);
    }

    QStringList usable;
    for (const auto& path : matches) {
        if (kdeDrmConnectorDirUsable(path)) usable << path;
    }
    if (usable.size() != 1) return {};
    return kdeReadValidEdidFile(usable.first());
}

inline KdeScreenIdentity makeKdeScreenIdentity(const QString& manufacturer, const QString& model,
                                               const QString& serial, const QString& connector,
                                               const QByteArray& edid) {
    const auto mfg  = manufacturer.trimmed();
    const auto mdl  = model.trimmed();
    const auto ser  = serial.trimmed();
    const auto conn = connector.trimmed();

    QByteArray        payload;
    KdeScreenIdentity identity;
    if (kdeEdidLooksValid(edid)) {
        payload         = edid;
        identity.source = KdeScreenIdentity::Source::Edid;
    } else if (! ser.isEmpty()) {
        payload = QStringLiteral("manufacturer=%1|model=%2|serial=%3").arg(mfg, mdl, ser).toUtf8();
        identity.source = KdeScreenIdentity::Source::Serial;
    } else if (! conn.isEmpty()) {
        payload         = QStringLiteral("connector=%1").arg(conn).toUtf8();
        identity.source = KdeScreenIdentity::Source::Connector;
    } else {
        return identity;
    }

    const auto md5 = QCryptographicHash::hash(payload, QCryptographicHash::Md5).toHex();
    identity.id    = QStringLiteral("kde-") + QString::fromLatin1(md5);
    return identity;
}
