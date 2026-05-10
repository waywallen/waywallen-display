/*
    SPDX-License-Identifier: GPL-2.0-or-later
*/

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as QQC2
import org.kde.plasma.plasmoid
import org.kde.kirigami as Kirigami

WallpaperItem {
    id: root

    // Prefer the EDID-derived "Manufacturer Model" string (stable across
    // port re-plugs) over the connector name like "DP-1" (which the
    // compositor reassigns when cables move). Fall back to the connector
    // name, then to a generic literal.
    readonly property string defaultDisplayName: {
        const m = (Screen.manufacturer || "").trim();
        const x = (Screen.model || "").trim();
        const vendor = [m, x].filter(v => v.length > 0).join(" ");
        if (vendor.length > 0) return vendor;
        if (Screen.name && Screen.name.length > 0) return Screen.name;
        return "kde-wallpaper";
    }

    // RFC 4122 v4 (random) UUID generated client-side on first run and
    // persisted in this wallpaper containment's KDE config. Sent to the
    // daemon as `register_display.instance_id` so per-display settings
    // (fillmode/align/clear color) live under a key that is stable
    // across screen renames, identical-monitor swaps, and user edits to
    // the human-readable Display name.
    function _generateUuidV4() {
        // Math.random isn't crypto-grade; here it just needs to make
        // collisions astronomically unlikely within one user's session
        // graveyard, which 122 bits of entropy comfortably covers.
        return "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx".replace(/[xy]/g, function (c) {
            const r = Math.random() * 16 | 0;
            const v = c === "x" ? r : (r & 0x3 | 0x8);
            return v.toString(16);
        });
    }

    // Background is owned by the renderer (via the daemon's
    // `set_config.clear_*`); the surface itself paints any letterbox
    // bars. Show opaque black until the surface attaches so a slow
    // daemon doesn't flash the desktop wallpaper through.
    Rectangle {
        anchors.fill: parent
        color: "black"
    }

    readonly property string surfaceSource: root.configuration.SurfaceMode === "system"
        ? "WaywallenSurface.qml"
        : "WaywallenSurfaceEmbed.qml"

    property bool _initDone: false

    Loader {
        id: surfaceLoader
        anchors.fill: parent
        asynchronous: false
        active: root._initDone
        source: root.surfaceSource

        onLoaded: {
            item.displayNameBinding    = Qt.binding(() =>
                root.configuration.DisplayName.length > 0
                    ? root.configuration.DisplayName
                    : root.defaultDisplayName);
            item.instanceIdBinding     = Qt.binding(() => root.configuration.DisplayInstanceId);
            item.displayWidthBinding   = Qt.binding(() => Math.round(root.width  * Screen.devicePixelRatio));
            item.displayHeightBinding  = Qt.binding(() => Math.round(root.height * Screen.devicePixelRatio));
            item.mouseForwardBinding   = Qt.binding(() => root.configuration.MouseForward);
        }
    }

    Loader {
        anchors.centerIn: parent
        width: Math.min(parent.width - 80, 720)
        active: surfaceLoader.status === Loader.Error
        visible: active
        sourceComponent: Rectangle {
            color: Qt.rgba(0, 0, 0, 0.7)
            radius: 8
            implicitHeight: col.implicitHeight + 32

            ColumnLayout {
                id: col
                anchors.fill: parent
                anchors.margins: 16
                spacing: 8

                QQC2.Label {
                    Layout.fillWidth: true
                    color: "white"
                    font.bold: true
                    font.pixelSize: 18
                    wrapMode: Text.WordWrap
                    text: root.configuration.SurfaceMode === "system"
                        ? i18nd("plasma_wallpaper_org.waywallen.kde",
                                "waywallen-display is not installed")
                        : i18nd("plasma_wallpaper_org.waywallen.kde",
                                "Embedded waywallen-display module failed to load")
                }
                QQC2.Label {
                    Layout.fillWidth: true
                    color: "white"
                    wrapMode: Text.WordWrap
                    text: root.configuration.SurfaceMode === "system"
                        ? i18nd("plasma_wallpaper_org.waywallen.kde",
                                "Install waywallen-display, or switch to Embedded mode in the wallpaper settings:")
                        : i18nd("plasma_wallpaper_org.waywallen.kde",
                                "The bundled module could not be loaded. Try switching to System mode after installing:")
                }
                QQC2.Label {
                    Layout.fillWidth: true
                    color: "#8ab4ff"
                    font.family: "monospace"
                    wrapMode: Text.WrapAnywhere
                    text: "https://github.com/waywallen/waywallen-display"
                }
            }
        }
    }

    Loader {
        anchors { top: parent.top; left: parent.left; margins: 12 }
        active: root.configuration.ShowDiagnostics && surfaceLoader.status === Loader.Ready
        sourceComponent: Text {
            color: "white"
            style: Text.Outline
            styleColor: "black"
            font.pixelSize: 14
            text: "waywallen: conn=" + surfaceLoader.item.connState
                  + " stream=" + surfaceLoader.item.streamState
                  + " frames=" + surfaceLoader.item.framesReceived
        }
    }

    Component.onCompleted: {
        if (root.configuration.DisplayInstanceId.length === 0) {
            root.configuration.DisplayInstanceId = root._generateUuidV4();
            root.configuration.writeConfig();
        }
        // Release the gate: surfaceLoader now constructs WaywallenDisplay
        // with a guaranteed-non-empty DisplayInstanceId, so the first
        // tryConnect carries the real UUID rather than racing the write.
        root._initDone = true;
        // Async wallpaper: content arrives via the daemon stream, which can
        // take arbitrary time (or never connect, if the daemon is down). We
        // must NOT gate ksplash on first frame — Plasma waits for every
        // wallpaper to flip `loading` to false before showing the desktop,
        // and a slow/missing daemon would freeze the whole session.
        // The clear-color rectangle is always visible while we wait.
        root.loading = false;
    }
}
