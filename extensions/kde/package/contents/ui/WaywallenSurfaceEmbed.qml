import QtQuick
import "WaywallenDisplayEmbed" as WW

WW.WaywallenDisplay {
    id: display

    property string displayNameBinding
    property string instanceIdBinding
    property color  clearColorBinding
    property int    displayWidthBinding
    property int    displayHeightBinding
    property bool   mouseForwardBinding: true

    displayName:         displayNameBinding
    instanceId:          instanceIdBinding
    clearColor:          clearColorBinding
    autoReconnect:       true
    displayWidth:        displayWidthBinding
    displayHeight:       displayHeightBinding
    mouseForwardEnabled: mouseForwardBinding

    signal firstFrameSeen()

    onFramesReceivedChanged: if (framesReceived === 1) firstFrameSeen()
}
