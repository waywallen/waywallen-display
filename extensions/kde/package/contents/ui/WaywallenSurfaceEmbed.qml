import QtQuick
import "WaywallenDisplay" as WW

WW.WaywallenDisplay {
    id: display

    property string displayNameBinding
    property string instanceIdBinding
    property color  clearColorBinding
    property int    displayWidthBinding
    property int    displayHeightBinding

    displayName:   displayNameBinding
    instanceId:    instanceIdBinding
    clearColor:    clearColorBinding
    autoReconnect: true
    displayWidth:  displayWidthBinding
    displayHeight: displayHeightBinding

    signal firstFrameSeen()

    onFramesReceivedChanged: if (framesReceived === 1) firstFrameSeen()
}
