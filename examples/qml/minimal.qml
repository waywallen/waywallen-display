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

    Rectangle {
        anchors { left: parent.left; top: parent.top; margins: 12 }
        width:  diagText.implicitWidth + 16
        height: diagText.implicitHeight + 12
        color: Qt.rgba(0, 0, 0, 0.55)
        radius: 6

        Text {
            id: diagText
            x: 8; y: 6
            color: "#cdd6f4"
            font.pixelSize: 13
            font.family: "monospace"
            text: {
                let s = "name:   " + wallpaper.displayName
                s += "  id: " + (wallpaper.displayId === 0
                                     ? "—"
                                     : wallpaper.displayId)
                s += "\nscreen: " + Screen.name + screenVendor()
                s += "\n  geom:  " + Screen.width + "x" + Screen.height
                      + " @ (" + Screen.virtualX + "," + Screen.virtualY + ")"
                s += "\n  avail: " + Screen.desktopAvailableWidth
                      + "x" + Screen.desktopAvailableHeight
                s += "\n  dpr=" + Screen.devicePixelRatio
                      + "  density=" + Screen.pixelDensity.toFixed(2) + " px/mm"
                s += "\n  orient: " + orientText(Screen.orientation)
                s += "\nconn:   " + connText(wallpaper.connState)
                      + "  stream: " + streamText(wallpaper.streamState)
                s += "\nframes: " + wallpaper.framesReceived
                if (wallpaper.lastDisconnectReason !== WaywallenDisplay.None) {
                    s += "\nreason: " + reasonText(wallpaper.lastDisconnectReason)
                    if (wallpaper.lastDisconnectMessage.length > 0)
                        s += "\n  msg:  " + wallpaper.lastDisconnectMessage
                }
                return s
            }

            function reasonText(r) {
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

            function screenVendor() {
                const m = (Screen.manufacturer || "").trim()
                const x = (Screen.model || "").trim()
                if (!m && !x) return ""
                return " [" + [m, x].filter(s => s.length > 0).join(" ") + "]"
            }

            function connText(st) {
                switch (st) {
                case WaywallenDisplay.Disconnected: return "disconnected"
                case WaywallenDisplay.Connecting:   return "connecting…"
                case WaywallenDisplay.Connected:    return "connected"
                case WaywallenDisplay.Error:        return "error"
                }
                return "unknown"
            }

            function streamText(st) {
                switch (st) {
                case WaywallenDisplay.Inactive: return "inactive"
                case WaywallenDisplay.Active:   return "active"
                }
                return "unknown"
            }

            function orientText(o) {
                switch (o) {
                case Qt.PrimaryOrientation:          return "primary"
                case Qt.PortraitOrientation:         return "portrait"
                case Qt.LandscapeOrientation:        return "landscape"
                case Qt.InvertedPortraitOrientation: return "portrait (inv)"
                case Qt.InvertedLandscapeOrientation:return "landscape (inv)"
                }
                return "?"
            }
        }
    }
}
