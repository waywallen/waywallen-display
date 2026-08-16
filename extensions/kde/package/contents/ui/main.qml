import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as QQC2
import org.kde.plasma.plasmoid
import org.kde.kirigami as Kirigami

WallpaperItem {
    id: root

    // Prefer the EDID-derived model (stable across port re-plugs) over the
    // connector name like "DP-1" (which the compositor reassigns when cables
    // move). Fall back to the connector name, then to a generic literal.
    readonly property string defaultDisplayName: {
        const model = (Screen.model || "").trim();
        if (model.length > 0) return model;
        if (Screen.name && Screen.name.length > 0) return Screen.name;
        return "kde-wallpaper";
    }

    // Background is owned by the renderer (via the daemon's
    // the composition clear color); the surface itself paints any letterbox
    // bars. Show opaque black until the surface attaches so a slow
    // daemon doesn't flash the desktop wallpaper through.
    Rectangle {
        anchors.fill: parent
        color: "black"
    }

    property bool _initDone: false
    readonly property bool accentColorFromWallpaper: root.configuration
        ? root.configuration.AccentColorFromWallpaper
        : false

    function scheduleAccentColorRefresh() {
        if (!root.accentColorFromWallpaper) {
            accentColorRefreshTimer.stop();
            return;
        }
        accentColorRefreshTimer.restart();
    }

    function refreshAccentColor() {
        if (Qt.colorEqual(root.accentColor, "transparent")) {
            root.accentColorChanged();
        } else {
            root.accentColor = "transparent";
        }
    }

    Timer {
        id: accentColorRefreshTimer
        interval: 2000
        repeat: false
        onTriggered: root.refreshAccentColor()
    }

    onAccentColorFromWallpaperChanged: {
        if (root.accentColorFromWallpaper) {
            root.scheduleAccentColorRefresh();
        } else {
            accentColorRefreshTimer.stop();
        }
    }

    // Per-display window-state reporter. We aggregate the current
    // screen's covering windows into a bitmask and push it down to
    // the daemon via WaywallenDisplay.windowStateFlags. The daemon
    // owns the autopause policy; this side only reports raw facts.
    WindowModel {
        id: windowModel
        // TasksModel.filterByScreen expects geometry in the compositor's
        // global virtual-desktop coords. WallpaperItem's own x/y are local
        // to its containment parent — both screens report (0, 0) there, so
        // every wallpaper instance ends up matching the same set of windows.
        // Screen.virtualX/Y is the per-screen global origin, which is what
        // makes the per-display filter actually differentiate.
        screenGeometry: Qt.rect(Screen.virtualX, Screen.virtualY,
                                Screen.width, Screen.height)
    }

    Loader {
        id: surfaceLoader
        anchors.fill: parent
        asynchronous: false
        active: root._initDone
        source: "WaywallenSurface.qml"

        onLoaded: {
            const d = item.display;
            d.displayName = Qt.binding(() =>
                root.configuration.DisplayName.length > 0
                    ? root.configuration.DisplayName
                    : root.defaultDisplayName);
            d.displayWidth = Qt.binding(() => Math.round(root.width * Screen.devicePixelRatio));
            d.displayHeight = Qt.binding(() => Math.round(root.height * Screen.devicePixelRatio));
            d.mouseForwardEnabled = Qt.binding(() => root.configuration.MouseForward);
            d.windowStateFlags = Qt.binding(() => windowModel.flags);
            d.contentRevisionChanged.connect(root.scheduleAccentColorRefresh);
        }
    }

    Loader {
        anchors.centerIn: parent
        width: Math.min(parent.width - 80, 720)
        active: surfaceLoader.status === Loader.Error
        visible: active
        sourceComponent: Rectangle {
            color: Qt.rgba(0, 0, 0, 0.7)
            radius: 8
            implicitHeight: col.implicitHeight + 32

            ColumnLayout {
                id: col
                anchors.fill: parent
                anchors.margins: 16
                spacing: 8

                QQC2.Label {
                    Layout.fillWidth: true
                    color: "white"
                    font.bold: true
                    font.pixelSize: 18
                    wrapMode: Text.WordWrap
                    text: i18nd("plasma_wallpaper_org.waywallen.kde",
                                "waywallen-display module failed to load")
                }
                QQC2.Label {
                    Layout.fillWidth: true
                    color: "white"
                    wrapMode: Text.WordWrap
                    text: i18nd("plasma_wallpaper_org.waywallen.kde",
                                "Install or update waywallen-display:")
                }
                QQC2.Label {
                    Layout.fillWidth: true
                    color: "#8ab4ff"
                    font.family: "monospace"
                    wrapMode: Text.WrapAnywhere
                    text: "https://github.com/waywallen/waywallen-display"
                }
            }
        }
    }

    Loader {
        anchors { top: parent.top; left: parent.left; margins: 12 }
        active: root.configuration.ShowDiagnostics && surfaceLoader.status === Loader.Ready
        sourceComponent: Rectangle {
            id: diagBox
            width:  diagText.implicitWidth + 16
            height: diagText.implicitHeight + 12
            color: Qt.rgba(0, 0, 0, 0.55)
            radius: 6

            readonly property var surface: surfaceLoader.item
            readonly property var display: surface ? surface.display : null

            Text {
                id: diagText
                x: 8; y: 6
                color: "#cdd6f4"
                font.pixelSize: 13
                font.family: "monospace"
                text: {
                    const d = diagBox.display;
                    if (!d) return ""
                    let s = i18nd("plasma_wallpaper_org.waywallen.kde", "name:") + "   " + d.displayName
                    s += "  " + i18nd("plasma_wallpaper_org.waywallen.kde", "id:") + " " + (d.displayId === 0 ? "—" : d.displayId)
                    s += "\n" + i18nd("plasma_wallpaper_org.waywallen.kde", "inst:") + "   " + (d.instanceId.length > 0 ? d.instanceId : "—")
                    s += "  " + i18nd("plasma_wallpaper_org.waywallen.kde", "source:") + " " + (d.instanceIdSource.length > 0 ? d.instanceIdSource : "—")
                    s += "\n" + i18nd("plasma_wallpaper_org.waywallen.kde", "screen:") + " " + Screen.name + screenVendor()
                    s += "\n  " + i18nd("plasma_wallpaper_org.waywallen.kde", "geom:") + "  " + Screen.width + "x" + Screen.height
                          + " @ (" + Screen.virtualX + "," + Screen.virtualY + ")"
                    s += "\n  " + i18nd("plasma_wallpaper_org.waywallen.kde", "avail:") + " " + Screen.desktopAvailableWidth
                          + "x" + Screen.desktopAvailableHeight
                    s += "\n  dpr=" + Screen.devicePixelRatio
                          + "  density=" + Screen.pixelDensity.toFixed(2) + " px/mm"
                    s += "\n  " + i18nd("plasma_wallpaper_org.waywallen.kde", "orient:") + " " + orientText(Screen.orientation)
                    s += "\n" + i18nd("plasma_wallpaper_org.waywallen.kde", "conn:") + "   " + connText(d.connState)
                          + "  " + i18nd("plasma_wallpaper_org.waywallen.kde", "stream:") + " " + streamText(d.streamState)
                    s += "\n" + i18nd("plasma_wallpaper_org.waywallen.kde", "frames:") + " " + d.framesReceived
                    s += "\n" + i18nd("plasma_wallpaper_org.waywallen.kde", "clear:") + "  " + d.clearColor.toString()
                    s += "\n  pause-effect: cfg=" + d.presentationConfigGeneration
                          + " dyn=" + d.presentationStateGeneration
                          + " kind=" + d.pauseEffectKind
                          + " active=" + d.pauseEffectActive
                          + " loaded=" + diagBox.surface.blurLoaded
                          + " radius=" + d.blurRadius
                    s += "\n" + i18nd("plasma_wallpaper_org.waywallen.kde", "windows:") + " " + windowsText()
                    if (d.lastDisconnectReason !== 0) {
                        s += "\n" + i18nd("plasma_wallpaper_org.waywallen.kde", "reason:") + " " + reasonText(d.lastDisconnectReason)
                        if (d.lastDisconnectMessage.length > 0)
                            s += "\n  " + i18nd("plasma_wallpaper_org.waywallen.kde", "msg:") + "  " + d.lastDisconnectMessage
                    }
                    return s
                }

                // Renders the same screen-filtered window list that drives
                // the autopause bitmask, so the displayed state and the
                // reported flags can be cross-checked at a glance.
                function windowsText() {
                    const f = windowModel.flags;
                    const ws = windowModel.windows;
                    let s = ws.length + " flags=0x" + f.toString(16)
                          + " [" + flagsBits(f) + "]"
                    if (ws.length === 0) return s
                    for (let i = 0; i < ws.length; i++) {
                        const w = ws[i]
                        const label = (w.app && w.app.length > 0)
                            ? w.app + " — " + w.title
                            : w.title
                        s += "\n  [" + winTag(w) + "] " + (label || i18nd("plasma_wallpaper_org.waywallen.kde", "(untitled)"))
                    }
                    return s
                }

                // Bit layout mirrors WAYWALLEN_WIN_HAS_* in
                // waywallen_display.h (see WindowModel.qml).
                function flagsBits(f) {
                    if (f === 0) return "—"
                    const names = []
                    if (f & 1) names.push(i18nd("plasma_wallpaper_org.waywallen.kde", "nonmin"))
                    if (f & 2) names.push(i18nd("plasma_wallpaper_org.waywallen.kde", "active"))
                    if (f & 4) names.push(i18nd("plasma_wallpaper_org.waywallen.kde", "max"))
                    if (f & 8) names.push(i18nd("plasma_wallpaper_org.waywallen.kde", "full"))
                    return names.join("+")
                }

                function winTag(w) {
                    let t = ""
                    t += w.minimized  ? "m" : "-"
                    t += w.active     ? "A" : "-"
                    t += w.fullscreen ? "F" : (w.maximized ? "M" : "-")
                    return t
                }

                // Enum values mirror WaywallenDisplay::DisconnectReason in
                // plugins/qml/WaywallenDisplay.hpp.
                function reasonText(r) {
                    switch (r) {
                    case 0: return "—"
                    case 1: return i18nd("plasma_wallpaper_org.waywallen.kde", "version unsupported")
                    case 2: return i18nd("plasma_wallpaper_org.waywallen.kde", "protocol mismatch")
                    case 3: return i18nd("plasma_wallpaper_org.waywallen.kde", "daemon error")
                    case 4: return i18nd("plasma_wallpaper_org.waywallen.kde", "handshake failed")
                    case 5: return i18nd("plasma_wallpaper_org.waywallen.kde", "socket io")
                    case 6: return i18nd("plasma_wallpaper_org.waywallen.kde", "protocol error")
                    case 7: return i18nd("plasma_wallpaper_org.waywallen.kde", "daemon gone")
                    }
                    return i18nd("plasma_wallpaper_org.waywallen.kde", "unknown")
                }

                function screenVendor() {
                    const m = (Screen.manufacturer || "").trim()
                    const x = (Screen.model || "").trim()
                    if (!m && !x) return ""
                    return " [" + [m, x].filter(s => s.length > 0).join(" ") + "]"
                }

                // Mirrors WaywallenDisplay::ConnState.
                function connText(st) {
                    switch (st) {
                    case 0: return i18nd("plasma_wallpaper_org.waywallen.kde", "disconnected")
                    case 1: return i18nd("plasma_wallpaper_org.waywallen.kde", "connecting…")
                    case 2: return i18nd("plasma_wallpaper_org.waywallen.kde", "handshaking…")
                    case 3: return i18nd("plasma_wallpaper_org.waywallen.kde", "connected")
                    case 4: return i18nd("plasma_wallpaper_org.waywallen.kde", "error")
                    }
                    return i18nd("plasma_wallpaper_org.waywallen.kde", "unknown")
                }

                // Mirrors WaywallenDisplay::StreamState.
                function streamText(st) {
                    switch (st) {
                    case 0: return i18nd("plasma_wallpaper_org.waywallen.kde", "inactive")
                    case 1: return i18nd("plasma_wallpaper_org.waywallen.kde", "active")
                    }
                    return i18nd("plasma_wallpaper_org.waywallen.kde", "unknown")
                }

                function orientText(o) {
                    switch (o) {
                    case Qt.PrimaryOrientation:           return i18nd("plasma_wallpaper_org.waywallen.kde", "primary")
                    case Qt.PortraitOrientation:          return i18nd("plasma_wallpaper_org.waywallen.kde", "portrait")
                    case Qt.LandscapeOrientation:         return i18nd("plasma_wallpaper_org.waywallen.kde", "landscape")
                    case Qt.InvertedPortraitOrientation:  return i18nd("plasma_wallpaper_org.waywallen.kde", "portrait (inv)")
                    case Qt.InvertedLandscapeOrientation: return i18nd("plasma_wallpaper_org.waywallen.kde", "landscape (inv)")
                    }
                    return "?"
                }
            }
        }
    }

    Component.onCompleted: {
        root._initDone = true;
        // Async wallpaper: content arrives via the daemon stream, which can
        // take arbitrary time (or never connect, if the daemon is down). We
        // must NOT gate ksplash on first frame — Plasma waits for every
        // wallpaper to flip `loading` to false before showing the desktop,
        // and a slow/missing daemon would freeze the whole session.
        // The clear-color rectangle is always visible while we wait.
        root.loading = false;
    }
}
