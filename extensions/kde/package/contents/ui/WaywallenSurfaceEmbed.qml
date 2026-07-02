import QtQuick
import "WaywallenDisplayEmbed" as WW

WW.WaywallenDisplay {
    id: display

    property string displayNameBinding
    property int    displayWidthBinding
    property int    displayHeightBinding
    property bool   mouseForwardBinding: true
    property int    windowStateFlagsBinding: 0

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
