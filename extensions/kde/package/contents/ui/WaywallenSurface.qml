import QtQuick
import QtQuick.Effects
import Waywallen.Display as WW

Item {
    id: root

    property string displayNameBinding
    property int    displayWidthBinding
    property int    displayHeightBinding
    property bool   mouseForwardBinding: true
    property int    windowStateFlagsBinding: 0

    readonly property alias displayName: display.displayName
    readonly property alias instanceId: display.instanceId
    readonly property alias displayId: display.displayId
    readonly property alias connState: display.connState
    readonly property alias streamState: display.streamState
    readonly property alias framesReceived: display.framesReceived
    readonly property alias clearColor: display.clearColor
    readonly property alias lastDisconnectReason: display.lastDisconnectReason
    readonly property alias lastDisconnectMessage: display.lastDisconnectMessage
    readonly property alias pauseEffectKind: display.pauseEffectKind
    readonly property alias blurRadius: display.blurRadius
    readonly property alias pauseEffectActive: display.pauseEffectActive
    readonly property alias presentationConfigGeneration: display.presentationConfigGeneration
    readonly property alias presentationDynamicGeneration: display.presentationDynamicGeneration
    readonly property bool blurLoaded: blurLoader.status === Loader.Ready

    signal firstFrameSeen()
    signal contentSourceChanged()

    WW.WaywallenDisplay {
        id: display
        anchors.fill: parent

        displayName: displayNameBinding
        autoReconnect: true
        displayWidth: displayWidthBinding
        displayHeight: displayHeightBinding
        mouseForwardEnabled: mouseForwardBinding
        windowStateFlags: windowStateFlagsBinding
        presentationCapabilities: WW.WaywallenDisplay.BlurCapability

        onFramesReceivedChanged: if (framesReceived === 1) root.firstFrameSeen()
        onContentRevisionChanged: root.contentSourceChanged()
    }

    Loader {
        id: blurLoader
        anchors.fill: parent
        active: display.pauseEffectKind === WW.WaywallenDisplay.BlurPauseEffect
        asynchronous: false

        sourceComponent: MultiEffect {
            anchors.fill: parent
            source: display
            autoPaddingEnabled: false
            blurEnabled: true
            blurMax: 64
            blur: display.pauseEffectActive ? display.blurRadius / 64.0 : 0.0
            visible: display.pauseEffectActive || blur > 0.0

            Behavior on blur {
                NumberAnimation { duration: 180; easing.type: Easing.OutCubic }
            }
        }
    }
}
