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

    readonly property string defaultDisplayName: {
        const screen = root.parent && root.parent.screen ? root.parent.screen.name : "";
        return screen.length > 0 ? ("kde-wallpaper-" + screen) : "kde-wallpaper";
    }

    Rectangle {
        anchors.fill: parent
        color: root.configuration.ClearColor
    }

    Loader {
        id: surfaceLoader
        anchors.fill: parent
        asynchronous: false
        source: "WaywallenSurface.qml"

        onLoaded: {
            item.displayNameBinding    = Qt.binding(() =>
                root.configuration.DisplayName.length > 0
                    ? root.configuration.DisplayName
                    : root.defaultDisplayName);
            item.clearColorBinding     = Qt.binding(() => root.configuration.ClearColor);
            item.autoReconnectBinding  = Qt.binding(() => root.configuration.AutoReconnect);
            item.displayWidthBinding   = Qt.binding(() => Math.round(root.width  * Screen.devicePixelRatio));
            item.displayHeightBinding  = Qt.binding(() => Math.round(root.height * Screen.devicePixelRatio));
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
                    text: i18nd("plasma_wallpaper_org.waywallen.kde",
                                "waywallen-display is not installed")
                }
                QQC2.Label {
                    Layout.fillWidth: true
                    color: "white"
                    wrapMode: Text.WordWrap
                    text: i18nd("plasma_wallpaper_org.waywallen.kde",
                                "This wallpaper needs the waywallen-display library. " +
                                "Please install it first:")
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
        // Async wallpaper: content arrives via the daemon stream, which can
        // take arbitrary time (or never connect, if the daemon is down). We
        // must NOT gate ksplash on first frame — Plasma waits for every
        // wallpaper to flip `loading` to false before showing the desktop,
        // and a slow/missing daemon would freeze the whole session.
        // The clear-color rectangle is always visible while we wait.
        root.loading = false;
    }

    Connections {
        target: surfaceLoader
        function onStatusChanged() {
            if (surfaceLoader.status === Loader.Error) {
                console.warn("waywallen-kde: failed to load Waywallen.Display —",
                             surfaceLoader.sourceComponent);
            }
        }
    }
}
