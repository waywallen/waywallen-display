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
    property string instanceIdBinding
    property int    displayWidthBinding
    property int    displayHeightBinding
    property bool   mouseForwardBinding: true

    // `clearColor` is owned by the renderer; the QML module reads it
    // from the daemon's `set_config` and applies it internally. KDE's
    // wallpaper plugin no longer overrides it.
    displayName:         displayNameBinding
    instanceId:          instanceIdBinding
    autoReconnect:       true
    displayWidth:        displayWidthBinding
    displayHeight:       displayHeightBinding
    mouseForwardEnabled: mouseForwardBinding

    signal firstFrameSeen()

    onFramesReceivedChanged: if (framesReceived === 1) firstFrameSeen()
}
