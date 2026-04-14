/*
 * with_controls.qml — WaywallenDisplay with runtime property controls.
 *
 * Run:
 *   QML_IMPORT_PATH=/path/to/plugin/build qml with_controls.qml
 */

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Waywallen.Display

ApplicationWindow {
    id: root
    width: 1280
    height: 720
    visible: true
    title: "waywallen — controls"
    color: "#1e1e2e"

    WaywallenDisplay {
        id: wallpaper
        anchors.fill: parent
        displayName: nameField.text
        displayWidth: root.width
        displayHeight: root.height
        autoReconnect: reconnectToggle.checked
        clearColor: colorField.text
    }

    // Semi-transparent control panel.
    Pane {
        anchors { right: parent.right; top: parent.top; margins: 16 }
        width: 280
        opacity: 0.9
        background: Rectangle { color: "#313244"; radius: 8 }

        ColumnLayout {
            anchors.fill: parent
            spacing: 10

            Label {
                text: "WaywallenDisplay Controls"
                font.bold: true
                font.pixelSize: 15
                color: "#cdd6f4"
            }

            // --- Status ---
            Label {
                color: "#a6adc8"
                text: "Conn: " + connText(wallpaper.connState)
                      + "  Stream: " + streamText(wallpaper.streamState)
                function connText(st) {
                    switch (st) {
                    case WaywallenDisplay.Disconnected: return "Disconnected"
                    case WaywallenDisplay.Connecting:   return "Connecting…"
                    case WaywallenDisplay.Connected:    return "Connected"
                    case WaywallenDisplay.Error:        return "Error"
                    }
                    return "Unknown"
                }
                function streamText(st) {
                    switch (st) {
                    case WaywallenDisplay.Inactive: return "Inactive"
                    case WaywallenDisplay.Active:   return "Active"
                    }
                    return "Unknown"
                }
            }

            Label {
                color: "#a6adc8"
                text: "Frames: " + wallpaper.framesReceived
            }

            // --- Display name ---
            Label { color: "#a6adc8"; text: "Display name:" }
            TextField {
                id: nameField
                Layout.fillWidth: true
                text: "qml-controls"
                color: "#cdd6f4"
                background: Rectangle { color: "#45475a"; radius: 4 }
            }

            // --- Socket path ---
            Label { color: "#a6adc8"; text: "Socket path (empty = default):" }
            TextField {
                id: socketField
                Layout.fillWidth: true
                placeholderText: "$XDG_RUNTIME_DIR/waywallen/display.sock"
                color: "#cdd6f4"
                background: Rectangle { color: "#45475a"; radius: 4 }
                onTextChanged: wallpaper.socketPath = text
            }

            // --- Clear color ---
            Label { color: "#a6adc8"; text: "Clear color:" }
            TextField {
                id: colorField
                Layout.fillWidth: true
                text: "#1e1e2e"
                color: "#cdd6f4"
                background: Rectangle { color: "#45475a"; radius: 4 }
            }

            // --- Auto-reconnect ---
            Switch {
                id: reconnectToggle
                text: "Auto reconnect"
                checked: true
                palette.text: "#cdd6f4"
            }
        }
    }
}
