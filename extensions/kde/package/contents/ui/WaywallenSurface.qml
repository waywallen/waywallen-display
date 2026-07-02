import QtQuick
import Waywallen.Display as WW

WW.WaywallenDisplay {
    id: display

    property string displayNameBinding
    property int    displayWidthBinding
    property int    displayHeightBinding
    property bool   mouseForwardBinding: true
    property int    windowStateFlagsBinding: 0

    // `clearColor` is owned by the renderer; the QML module reads it
    // from the daemon's `set_config` and applies it internally. KDE's
    // wallpaper plugin no longer overrides it.
    displayName:         displayNameBinding
    autoReconnect:       true
    displayWidth:        displayWidthBinding
    displayHeight:       displayHeightBinding
    mouseForwardEnabled: mouseForwardBinding
    windowStateFlags:    windowStateFlagsBinding

    signal firstFrameSeen()
    signal contentSourceChanged()

    onFramesReceivedChanged: if (framesReceived === 1) firstFrameSeen()
    onContentRevisionChanged: contentSourceChanged()
}
