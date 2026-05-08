/*
    SPDX-License-Identifier: GPL-2.0-or-later

    Probe stub for the system-wide `Waywallen.Display` QML module.
    Component compilation fails iff the import cannot be resolved;
    instantiating PluginInfo lets config.qml read the libdisplay
    version off the created object.
*/

import QtQuick
import Waywallen.Display as WW

WW.PluginInfo {}
