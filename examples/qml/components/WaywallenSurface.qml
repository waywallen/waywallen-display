import QtQuick
import QtQuick.Effects
import Waywallen.Display as WW

Item {
    id: root

    property alias display: displayItem
    property alias socketPath: displayItem.socketPath
    property alias displayName: displayItem.displayName
    property alias instanceId: displayItem.instanceId
    property alias displayWidth: displayItem.displayWidth
    property alias displayHeight: displayItem.displayHeight
    property alias autoReconnect: displayItem.autoReconnect
    property alias mouseForwardEnabled: displayItem.mouseForwardEnabled
    property alias windowStateFlags: displayItem.windowStateFlags

    property alias displayId: displayItem.displayId
    property alias connState: displayItem.connState
    property alias streamState: displayItem.streamState
    property alias framesReceived: displayItem.framesReceived
    property alias clearColor: displayItem.clearColor
    property alias lastDisconnectReason: displayItem.lastDisconnectReason
    property alias lastDisconnectMessage: displayItem.lastDisconnectMessage
    property alias pauseEffectKind: displayItem.pauseEffectKind
    property alias pauseEffectActive: displayItem.pauseEffectActive
    property alias blurRadius: displayItem.blurRadius
    readonly property bool blurLoaded: blurLoader.status === Loader.Ready

    WW.WaywallenDisplay {
        id: displayItem
        anchors.fill: parent
        displayWidth: root.width
        displayHeight: root.height
        presentationCapabilities: WW.WaywallenDisplay.BlurCapability
    }

    Loader {
        id: blurLoader
        anchors.fill: parent
        active: displayItem.pauseEffectKind === WW.WaywallenDisplay.BlurPauseEffect
        asynchronous: false

        sourceComponent: MultiEffect {
            anchors.fill: parent
            source: displayItem
            autoPaddingEnabled: false
            blurEnabled: true
            blurMax: 64
            blur: displayItem.pauseEffectActive ? displayItem.blurRadius / 64.0 : 0.0
            visible: displayItem.pauseEffectActive || blur > 0.0

            Behavior on blur {
                NumberAnimation { duration: 180; easing.type: Easing.OutCubic }
            }
        }
    }
}
