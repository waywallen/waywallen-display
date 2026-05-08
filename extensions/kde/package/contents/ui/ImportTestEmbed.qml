/*
    SPDX-License-Identifier: GPL-2.0-or-later

    Probe stub for the bundled contents/ui/WaywallenDisplayEmbed/
    QML module. Component compilation fails iff the relative-directory
    import cannot be resolved; instantiating PluginInfo lets
    config.qml read the libdisplay version off the created object.
*/

import QtQuick
import "WaywallenDisplayEmbed" as WW

WW.PluginInfo {}
