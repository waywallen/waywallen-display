#include "ScreenIdentity.hpp"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <cassert>
#include <cstdio>

static QByteArray validEdid(char tag, int blocks = 1) {
    assert(blocks >= 1);
    QByteArray              bytes(128 * blocks, char(0));
    static constexpr quint8 kHeader[] = { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 };
    for (int i = 0; i < 8; ++i) bytes[i] = static_cast<char>(kHeader[i]);
    bytes[8] = tag;
    for (int block = 0; block < blocks; ++block) {
        quint8     sum = 0;
        const auto off = block * 128;
        for (int i = 0; i < 127; ++i) sum += static_cast<quint8>(bytes[off + i]);
        bytes[off + 127] = static_cast<char>(static_cast<quint8>(0u - sum));
    }
    assert(kdeEdidLooksValid(bytes));
    return bytes;
}

static void writeFile(const QString& path, const QByteArray& data) {
    QFile file(path);
    assert(file.open(QIODevice::WriteOnly));
    assert(file.write(data) == data.size());
}

static void test_serial_ignores_connector() {
    const auto a = makeKdeScreenIdentity("Dell", "U2720Q", "ABC", "DP-1", {});
    const auto b = makeKdeScreenIdentity("Dell", "U2720Q", "ABC", "HDMI-A-1", {});
    assert(a.source == KdeScreenIdentity::Source::Serial);
    assert(a.sourceName() == QStringLiteral("serial"));
    assert(a.id.startsWith(QStringLiteral("kde-")));
    assert(a.id == b.id);
}

static void test_serial_trims_and_differs_by_serial() {
    const auto left  = makeKdeScreenIdentity("  Dell ", "U2720Q", " AAA ", "DP-1", {});
    const auto right = makeKdeScreenIdentity("Dell", "U2720Q", "BBB", "DP-1", {});
    assert(left.source == KdeScreenIdentity::Source::Serial);
    assert(left.id != right.id);
}

static void test_edid_beats_serial() {
    const auto withSerial = makeKdeScreenIdentity("Dell", "U2720Q", "ABC", "DP-1", validEdid('A'));
    const auto otherSerial =
        makeKdeScreenIdentity("Dell", "U2720Q", "XYZ", "HDMI-A-1", validEdid('A'));
    const auto otherEdid =
        makeKdeScreenIdentity("Dell", "U2720Q", "ABC", "HDMI-A-1", validEdid('B'));
    assert(withSerial.source == KdeScreenIdentity::Source::Edid);
    assert(otherSerial.source == KdeScreenIdentity::Source::Edid);
    assert(withSerial.sourceName() == QStringLiteral("edid"));
    assert(withSerial.id == otherSerial.id);
    assert(otherEdid.source == KdeScreenIdentity::Source::Edid);
    assert(withSerial.id != otherEdid.id);
}

static void test_edid_used_when_serial_empty() {
    const auto sameA = makeKdeScreenIdentity("Dell", "U2720Q", "  ", "DP-1", validEdid('A'));
    const auto sameB =
        makeKdeScreenIdentity("Dell", "U2720Q", QString(), "HDMI-A-1", validEdid('A'));
    const auto other = makeKdeScreenIdentity("Dell", "U2720Q", QString(), "DP-1", validEdid('B'));
    assert(sameA.source == KdeScreenIdentity::Source::Edid);
    assert(sameA.sourceName() == QStringLiteral("edid"));
    assert(sameA.id == sameB.id);
    assert(sameA.id != other.id);
}

static void test_connector_last_resort() {
    const auto emptyEdid = QByteArray(128, char(0));
    const auto a         = makeKdeScreenIdentity("Dell", "U2720Q", QString(), "DP-1", emptyEdid);
    const auto b         = makeKdeScreenIdentity("Dell", "U2720Q", QString(), "HDMI-A-1", {});
    const auto same      = makeKdeScreenIdentity("LG", "foo", QString(), " DP-1 ", {});
    assert(a.source == KdeScreenIdentity::Source::Connector);
    assert(a.sourceName() == QStringLiteral("connector"));
    assert(a.id != b.id);
    assert(a.id == same.id);
}

static void test_empty_inputs_yield_no_identity() {
    const auto none = makeKdeScreenIdentity("  ", QString(), QString(), QString(), {});
    assert(none.source == KdeScreenIdentity::Source::None);
    assert(none.id.isEmpty());
    assert(none.sourceName().isEmpty());
}

static void test_edid_rejects_short_zero_header_and_checksum() {
    assert(! kdeEdidLooksValid({}));
    assert(! kdeEdidLooksValid(QByteArray("tiny")));
    assert(! kdeEdidLooksValid(QByteArray(200, char(1))));
    assert(! kdeEdidLooksValid(QByteArray(128, char(0))));

    auto badHeader = validEdid('A');
    badHeader[0]   = static_cast<char>(0x11);
    assert(! kdeEdidLooksValid(badHeader));

    auto badChecksum = validEdid('A');
    badChecksum[127] = static_cast<char>(static_cast<quint8>(badChecksum[127]) + 1);
    assert(! kdeEdidLooksValid(badChecksum));

    assert(kdeEdidLooksValid(validEdid('A')));
    assert(kdeEdidLooksValid(validEdid('B', 2)));
}

static void test_sysfs_exact_connector_path() {
    QTemporaryDir tmp;
    assert(tmp.isValid());
    const QString exact = tmp.path() + QStringLiteral("/DP-2");
    assert(QDir().mkpath(exact));
    const auto edid = validEdid('e');
    writeFile(exact + QStringLiteral("/edid"), edid);
    assert(kdeConnectedEdid(QStringLiteral("DP-2"), tmp.path()) == edid);
}

static void test_sysfs_edid_prefers_connected_output() {
    QTemporaryDir tmp;
    assert(tmp.isValid());
    const QString card0 = tmp.path() + QStringLiteral("/card0-DP-1");
    const QString card1 = tmp.path() + QStringLiteral("/card1-DP-1");
    assert(QDir().mkpath(card0));
    assert(QDir().mkpath(card1));
    writeFile(card0 + QStringLiteral("/status"), QByteArray("disconnected\n"));
    writeFile(card0 + QStringLiteral("/edid"), validEdid('x'));
    writeFile(card1 + QStringLiteral("/status"), QByteArray("connected\n"));
    writeFile(card1 + QStringLiteral("/edid"), validEdid('y'));

    const auto bytes = kdeConnectedEdid(QStringLiteral("DP-1"), tmp.path());
    assert(bytes == validEdid('y'));
}

static void test_sysfs_ambiguous_connected_cards_are_empty() {
    QTemporaryDir tmp;
    assert(tmp.isValid());
    const QString card0 = tmp.path() + QStringLiteral("/card0-DP-1");
    const QString card1 = tmp.path() + QStringLiteral("/card1-DP-1");
    assert(QDir().mkpath(card0));
    assert(QDir().mkpath(card1));
    writeFile(card0 + QStringLiteral("/status"), QByteArray("connected\n"));
    writeFile(card0 + QStringLiteral("/edid"), validEdid('x', 1));
    writeFile(card1 + QStringLiteral("/status"), QByteArray("connected\n"));
    writeFile(card1 + QStringLiteral("/edid"), validEdid('y', 2));
    assert(kdeConnectedEdid(QStringLiteral("DP-1"), tmp.path()).isEmpty());
}

static void test_sysfs_skips_short_or_zero_edid() {
    QTemporaryDir tmp;
    assert(tmp.isValid());
    const QString out = tmp.path() + QStringLiteral("/card0-HDMI-A-1");
    assert(QDir().mkpath(out));
    writeFile(out + QStringLiteral("/status"), QByteArray("connected\n"));
    writeFile(out + QStringLiteral("/edid"), QByteArray(128, char(0)));
    assert(kdeConnectedEdid(QStringLiteral("HDMI-A-1"), tmp.path()).isEmpty());

    writeFile(out + QStringLiteral("/edid"), QByteArray("tiny"));
    assert(kdeConnectedEdid(QStringLiteral("HDMI-A-1"), tmp.path()).isEmpty());
}

int main() {
    test_serial_ignores_connector();
    test_serial_trims_and_differs_by_serial();
    test_edid_beats_serial();
    test_edid_used_when_serial_empty();
    test_connector_last_resort();
    test_empty_inputs_yield_no_identity();
    test_edid_rejects_short_zero_header_and_checksum();
    test_sysfs_exact_connector_path();
    test_sysfs_edid_prefers_connected_output();
    test_sysfs_ambiguous_connected_cards_are_empty();
    test_sysfs_skips_short_or_zero_edid();
    std::puts("test_screen_identity: OK");
    return 0;
}
