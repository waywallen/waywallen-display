import QtQuick
import QtQuick.Effects
import "Plugin" as P

Item {
    property alias display: displayItem
    readonly property bool blurLoaded: blurLoader.status === Loader.Ready

    P.PluginDisplay {
        id: displayItem
        anchors.fill: parent

        autoReconnect: true
        presentationCapabilities: P.PluginDisplay.PauseBlurCapability
    }

    Loader {
        id: blurLoader
        anchors.fill: parent
        active: displayItem.pauseEffectKind === P.PluginDisplay.BlurPauseEffect
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
