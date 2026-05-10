/*
 * multi_display.qml — two independent WaywallenDisplay items
 * side by side, each registered as a separate display consumer.
 *
 * Run:
 *   QML_IMPORT_PATH=/path/to/plugin/build qml multi_display.qml
 */

import QtQuick
import QtQuick.Layouts
import Waywallen.Display

Window {
    id: root
    width: 1600
    height: 480
    visible: true
    title: "waywallen — multi display"
    color: "#1e1e2e"

    RowLayout {
        anchors.fill: parent
        spacing: 2

        // Left display.
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            WaywallenDisplay {
                id: left
                anchors.fill: parent
                displayName: "qml-left"
                displayWidth: parent.width
                displayHeight: parent.height
            }

            DiagBox {
                anchors { left: parent.left; top: parent.top; margins: 8 }
                wallpaper: left
            }
        }

        // Right display.
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            WaywallenDisplay {
                id: right
                anchors.fill: parent
                displayName: "qml-right"
                displayWidth: parent.width
                displayHeight: parent.height
            }

            DiagBox {
                anchors { left: parent.left; top: parent.top; margins: 8 }
                wallpaper: right
            }
        }
    }

    component DiagBox: Rectangle {
        id: box
        property var wallpaper

        width:  diagText.implicitWidth + 16
        height: diagText.implicitHeight + 12
        color: Qt.rgba(0, 0, 0, 0.55)
        radius: 6

        Label {
            id: diagText
            x: 8; y: 6
            color: "#cdd6f4"
            font.pixelSize: 13
            font.family: "monospace"
            text: {
                let s = box.wallpaper.displayName
                      + " (id=" + box.idLabel(box.wallpaper.displayId) + ") | "
                      + box.connLabel(box.wallpaper.connState)
                      + " " + box.streamLabel(box.wallpaper.streamState)
                      + " | frames: " + box.wallpaper.framesReceived
                      + "\nscreen: " + Screen.name + box.screenVendor()
                      + "\n  geom:  " + Screen.width + "x" + Screen.height
                              + " @ (" + Screen.virtualX + "," + Screen.virtualY + ")"
                      + "\n  avail: " + Screen.desktopAvailableWidth
                              + "x" + Screen.desktopAvailableHeight
                      + "\n  dpr=" + Screen.devicePixelRatio
                              + "  density=" + Screen.pixelDensity.toFixed(2) + " px/mm"
                      + "\nclear: " + box.wallpaper.clearColor.toString()
                if (box.wallpaper.lastDisconnectReason !== WaywallenDisplay.None) {
                    s += "\nreason: " + box.reasonLabel(box.wallpaper.lastDisconnectReason)
                    if (box.wallpaper.lastDisconnectMessage.length > 0)
                        s += "\n  msg:  " + box.wallpaper.lastDisconnectMessage
                }
                return s
            }
        }

        function screenVendor() {
            const m = (Screen.manufacturer || "").trim()
            const x = (Screen.model || "").trim()
            if (!m && !x) return ""
            return " [" + [m, x].filter(s => s.length > 0).join(" ") + "]"
        }

        function connLabel(st) {
            switch (st) {
            case WaywallenDisplay.Disconnected: return "disc"
            case WaywallenDisplay.Connecting:   return "conn…"
            case WaywallenDisplay.Connected:    return "conn"
            case WaywallenDisplay.Error:        return "err"
            }
            return "?"
        }

        function streamLabel(st) {
            switch (st) {
            case WaywallenDisplay.Inactive: return "inactive"
            case WaywallenDisplay.Active:   return "active"
            }
            return "?"
        }

        function idLabel(id) {
            return id === 0 ? "—" : id
        }

        function reasonLabel(r) {
            switch (r) {
            case WaywallenDisplay.None:               return "—"
            case WaywallenDisplay.VersionUnsupported: return "version unsupported"
            case WaywallenDisplay.ProtocolMismatch:   return "protocol mismatch"
            case WaywallenDisplay.DaemonError:        return "daemon error"
            case WaywallenDisplay.HandshakeFailed:    return "handshake failed"
            case WaywallenDisplay.SocketIo:           return "socket io"
            case WaywallenDisplay.ProtocolError:      return "protocol error"
            case WaywallenDisplay.DaemonGone:         return "daemon gone"
            }
            return "unknown"
        }
    }

    component Label: Text {}
}
