/*
 * minimal.qml — simplest possible WaywallenDisplay consumer.
 *
 * Run:
 *   qml minimal.qml
 *   # or
 *   qmlscene minimal.qml
 *
 * The plugin must be in QML_IMPORT_PATH or installed into the Qt
 * QML import tree. Example:
 *   QML_IMPORT_PATH=/path/to/waywallen-display/plugins/qml/build \
 *       qml minimal.qml
 */

import QtQuick
import Waywallen.Display

Window {
    id: root
    width: 1280
    height: 720
    visible: true
    title: "waywallen — minimal"
    color: "#1e1e2e"

    WaywallenDisplay {
        id: wallpaper
        anchors.fill: parent
        displayName: "qml-minimal"
        displayWidth: root.width
        displayHeight: root.height
    }

    // Status overlay (top-left).
    Text {
        anchors { left: parent.left; top: parent.top; margins: 12 }
        color: "#cdd6f4"
        font.pixelSize: 14
        font.family: "monospace"
        text: {
            let s = "status: " + statusText(wallpaper.status)
            s += "\nframes: " + wallpaper.framesReceived
            return s
        }

        function statusText(st) {
            switch (st) {
            case WaywallenDisplay.Disconnected: return "disconnected"
            case WaywallenDisplay.Connecting:   return "connecting…"
            case WaywallenDisplay.Idle:         return "idle"
            case WaywallenDisplay.Bound:        return "bound"
            case WaywallenDisplay.Error:        return "error"
            }
            return "unknown"
        }
    }
}
