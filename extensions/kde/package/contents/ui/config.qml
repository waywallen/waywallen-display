/*
    SPDX-License-Identifier: GPL-2.0-or-later
*/

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kquickcontrols as KQuickControls

ColumnLayout {
    id: root

    property string cfg_DisplayName
    property color  cfg_ClearColor
    property string cfg_SurfaceMode
    property bool   cfg_ShowDiagnostics

    property bool   _displayModuleAvailable: false

    Component.onCompleted: {
        // Probe Waywallen.Display without making a hard parse-time dependency.
        const probe = Qt.createQmlObject(
            'import QtQuick; import Waywallen.Display 1.0; QtObject {}',
            root, "waywallenProbe");
        _displayModuleAvailable = (probe !== null);
        if (probe) probe.destroy();
    }

    Kirigami.InlineMessage {
        Layout.fillWidth: true
        visible: cfg_SurfaceMode === "system" && !root._displayModuleAvailable
        type: Kirigami.MessageType.Warning
        text: i18nd("plasma_wallpaper_org.waywallen.kde",
                    "<b>waywallen-display</b> is not installed system-wide. " +
                    "Switch to <i>Embedded</i> mode or install the module from " +
                    "<a href=\"https://github.com/waywallen/waywallen-display\">" +
                    "github.com/waywallen/waywallen-display</a>.")
        onLinkActivated: Qt.openUrlExternally(link)
    }

    Kirigami.FormLayout {
        Layout.fillWidth: true
        twinFormLayouts: parentLayout

        QQC2.TextField {
            id: displayNameField
            Kirigami.FormData.label: i18nd("plasma_wallpaper_org.waywallen.kde", "Display name:")
            placeholderText: i18nd("plasma_wallpaper_org.waywallen.kde", "Auto-generated per screen")
            text: cfg_DisplayName
            onTextChanged: cfg_DisplayName = text
        }

        KQuickControls.ColorButton {
            Kirigami.FormData.label: i18nd("plasma_wallpaper_org.waywallen.kde", "Clear color:")
            color: cfg_ClearColor
            onColorChanged: cfg_ClearColor = color
            dialogTitle: i18nd("plasma_wallpaper_org.waywallen.kde", "Select clear color")
        }

        QQC2.ComboBox {
            Kirigami.FormData.label: i18nd("plasma_wallpaper_org.waywallen.kde", "Display module:")
            textRole: "label"
            valueRole: "value"
            model: [
                { value: "embed",  label: i18nd("plasma_wallpaper_org.waywallen.kde",
                                                "Embedded (bundled with this wallpaper)") },
                { value: "system", label: i18nd("plasma_wallpaper_org.waywallen.kde",
                                                "System (requires waywallen-display installed)") }
            ]
            currentIndex: cfg_SurfaceMode === "system" ? 1 : 0
            onActivated: cfg_SurfaceMode = currentValue
        }

        QQC2.CheckBox {
            Kirigami.FormData.label: i18nd("plasma_wallpaper_org.waywallen.kde", "Show diagnostics overlay")
            checked: cfg_ShowDiagnostics
            onToggled: cfg_ShowDiagnostics = checked
        }
    }

}
