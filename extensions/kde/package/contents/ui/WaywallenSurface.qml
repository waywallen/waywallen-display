/*
    SPDX-License-Identifier: GPL-2.0-or-later

    Isolated so that a missing `Waywallen.Display` module fails inside a Loader
    instead of killing the whole wallpaper plugin at parse time.
*/

import QtQuick
import Waywallen.Display as WW

WW.WaywallenDisplay {
    id: display

    property string displayNameBinding
    property color  clearColorBinding
    property bool   autoReconnectBinding
    property int    displayWidthBinding
    property int    displayHeightBinding

    displayName:   displayNameBinding
    clearColor:    clearColorBinding
    autoReconnect: autoReconnectBinding
    displayWidth:  displayWidthBinding
    displayHeight: displayHeightBinding

    signal firstFrameSeen()

    onFramesReceivedChanged: if (framesReceived === 1) firstFrameSeen()
}
