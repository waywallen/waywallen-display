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
                clearColor: "#1e1e2e"
            }

            Label {
                anchors { left: parent.left; top: parent.top; margins: 8 }
                color: "#cdd6f4"
                font.pixelSize: 13
                font.family: "monospace"
                text: "left  | " + statusLabel(left.status) + " | frames: " + left.framesReceived
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
                clearColor: "#181825"
            }

            Label {
                anchors { left: parent.left; top: parent.top; margins: 8 }
                color: "#cdd6f4"
                font.pixelSize: 13
                font.family: "monospace"
                text: "right | " + statusLabel(right.status) + " | frames: " + right.framesReceived
            }
        }
    }

    function statusLabel(st) {
        switch (st) {
        case WaywallenDisplay.Disconnected: return "disc"
        case WaywallenDisplay.Connecting:   return "conn…"
        case WaywallenDisplay.Idle:         return "idle"
        case WaywallenDisplay.Bound:        return "bound"
        case WaywallenDisplay.Error:        return "err"
        }
        return "?"
    }

    // Import the Label from QtQuick.Controls so we don't need a
    // separate import at top level.
    component Label: Text {}
}
