import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

ColumnLayout {
    id: root

    spacing: Kirigami.Units.largeSpacing

    property string cfg_DisplayName
    property bool   cfg_ShowDiagnostics
    property bool   cfg_MouseForward
    property bool   cfg_AccentColorFromWallpaper

    property string _probeError: ""
    property string _libVersion: ""

    // Compile a tiny stub QML that imports the display module. The stub
    // (ImportTest.qml) instantiates a PluginInfo from the QML module —
    // creating it serves a dual purpose: it both proves the import is
    // resolvable and exposes the libdisplay version to read here.
    function _probeSurface() {
        const c = Qt.createComponent("ImportTest.qml", Component.PreferSynchronous, root);
        if (!c) {
            root._probeError = "Failed to create QML component for ImportTest.qml";
            root._libVersion = "";
            return;
        }
        const finish = () => {
            if (c.status === Component.Error) {
                root._probeError = c.errorString();
                root._libVersion = "";
            } else if (c.status === Component.Ready) {
                root._probeError = "";
                const obj = c.createObject(root);
                if (obj) {
                    root._libVersion = obj.version || "";
                    obj.destroy();
                } else {
                    root._libVersion = "";
                }
            }
        };
        if (c.status === Component.Loading) {
            c.statusChanged.connect(finish);
        } else {
            finish();
        }
    }

    Component.onCompleted: root._probeSurface()

    Kirigami.FormLayout {
        Layout.fillWidth: true
        Layout.alignment: Qt.AlignTop
        twinFormLayouts: parentLayout

        QQC2.TextField {
            id: displayNameField
            Kirigami.FormData.label: i18nd("plasma_wallpaper_org.waywallen.kde", "Display name:")
            placeholderText: i18nd("plasma_wallpaper_org.waywallen.kde", "Auto-generated per screen")
            text: cfg_DisplayName
            onTextChanged: cfg_DisplayName = text
        }

        QQC2.CheckBox {
            Kirigami.FormData.label: i18nd("plasma_wallpaper_org.waywallen.kde", "Show diagnostics overlay")
            checked: cfg_ShowDiagnostics
            onToggled: cfg_ShowDiagnostics = checked
        }

        QQC2.CheckBox {
            Kirigami.FormData.label: i18nd("plasma_wallpaper_org.waywallen.kde", "Forward mouse events to wallpaper")
            checked: cfg_MouseForward
            onToggled: cfg_MouseForward = checked
        }

        QQC2.CheckBox {
            Kirigami.FormData.label: i18nd("plasma_wallpaper_org.waywallen.kde", "Accent color from wallpaper")
            checked: cfg_AccentColorFromWallpaper
            onToggled: cfg_AccentColorFromWallpaper = checked
        }

        QQC2.Label {
            Kirigami.FormData.label: i18nd("plasma_wallpaper_org.waywallen.kde", "Project:")
            text: "<a href=\"https://github.com/waywallen/waywallen\">github.com/waywallen/waywallen</a>"
            textFormat: Text.RichText
            onLinkActivated: (link) => Qt.openUrlExternally(link)
        }

        QQC2.Label {
            Kirigami.FormData.label: i18nd("plasma_wallpaper_org.waywallen.kde", "Version:")
            visible: root._libVersion.length > 0
            text: root._libVersion
        }
    }

    Kirigami.InlineMessage {
        Layout.fillWidth: true
        Layout.leftMargin: Kirigami.Units.largeSpacing
        Layout.rightMargin: Kirigami.Units.largeSpacing
        visible: true
        type: Kirigami.MessageType.Information
        text: i18nd("plasma_wallpaper_org.waywallen.kde",
                    "After upgrading the plugin, restart plasmashell to pick up the new QML module:<br/>" +
                    "<code>systemctl --user restart plasma-plasmashell.service</code>")
    }

    Kirigami.InlineMessage {
        Layout.fillWidth: true
        Layout.leftMargin: Kirigami.Units.largeSpacing
        Layout.rightMargin: Kirigami.Units.largeSpacing
        visible: Qt.platform.pluginName === "xcb"
        type: Kirigami.MessageType.Information
        text: i18nd("plasma_wallpaper_org.waywallen.kde",
                    "On X11 sessions (e.g. Steam Deck), plasmashell needs the EGL XCB backend " +
                    "to import the wallpaper's DMA-BUFs. Create " +
                    "<code>~/.config/systemd/user/plasma-plasmashell.service.d/override.conf</code> with:<br/>" +
                    "<pre>[Service]\nEnvironment=QT_XCB_GL_INTEGRATION=xcb_egl</pre>" +
                    "then run <code>systemctl --user daemon-reload</code> and restart plasmashell.")
    }

    // Spacer pushes the error block down and keeps the form anchored to
    // the top regardless of how the surrounding config view sizes us.
    Item {
        Layout.fillHeight: true
        Layout.fillWidth: true
    }

    Kirigami.InlineMessage {
        Layout.fillWidth: true
        Layout.leftMargin: Kirigami.Units.largeSpacing
        Layout.rightMargin: Kirigami.Units.largeSpacing
        visible: root._probeError.length > 0
        type: Kirigami.MessageType.Error
        text: i18nd("plasma_wallpaper_org.waywallen.kde",
                    "<b>Failed to load the display module.</b><br/>" +
                    "Please report at <a href=\"https://github.com/waywallen/waywallen-display/issues\">" +
                    "github.com/waywallen/waywallen-display/issues</a>")
        onLinkActivated: (link) => Qt.openUrlExternally(link)
    }

    QQC2.TextArea {
        Layout.fillWidth: true
        Layout.leftMargin: Kirigami.Units.largeSpacing
        Layout.rightMargin: Kirigami.Units.largeSpacing
        Layout.bottomMargin: Kirigami.Units.largeSpacing
        visible: root._probeError.length > 0
        readOnly: true
        wrapMode: TextEdit.Wrap
        textFormat: TextEdit.PlainText
        font.family: "monospace"
        font.pixelSize: Kirigami.Theme.smallFont.pixelSize
        selectByMouse: true
        text: root._probeError
    }

}
