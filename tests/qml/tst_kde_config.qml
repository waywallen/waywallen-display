import QtQuick
import QtTest
import org.kde.kirigami as Kirigami

TestCase {
    id: appearanceRoot

    name: "KdeWallpaperConfig"

    property alias parentLayout: parentForm
    property var configComponent
    property var wallpaper

    function i18nd(domain, message) {
        return message;
    }

    Kirigami.FormLayout {
        id: parentForm
    }

    SignalSpy {
        id: changedSpy
        signalName: "configurationChanged"
    }

    function initTestCase() {
        configComponent = Qt.createComponent(
            "../../extensions/kde/package/contents/ui/config.qml", Component.PreferSynchronous);
        compare(configComponent.status, Component.Ready, configComponent.errorString());
    }

    function cleanup() {
        changedSpy.target = null;
        changedSpy.clear();
        wallpaper = undefined;
    }

    function form(page) {
        for (const child of page.children) {
            if (child instanceof Kirigami.FormLayout) return child;
        }
        fail("Wallpaper configuration form was not created");
    }

    function test_hostProperties() {
        const configuration = {
            DisplayName: "Test display",
            DisplayNameDefault: "",
            ShowDiagnostics: true,
            ShowDiagnosticsDefault: false,
            MouseForward: false,
            MouseForwardDefault: true,
            AccentColorFromWallpaper: true,
            AccentColorFromWallpaperDefault: false
        };
        const properties = {
            configDialog: appearanceRoot,
            wallpaperConfiguration: configuration
        };
        const temporary = createTemporaryObject(configComponent, appearanceRoot, properties);
        verify(temporary !== null);
        compare(temporary.configDialog, appearanceRoot);
        compare(temporary.wallpaperConfiguration, configuration);
        for (const key of Object.keys(configuration)) {
            verify(("cfg_" + key) in temporary);
            properties["cfg_" + key] = configuration[key];
        }

        const page = createTemporaryObject(configComponent, appearanceRoot, properties);
        verify(page !== null);
        compare(page.configDialog, appearanceRoot);
        compare(page.wallpaperConfiguration, configuration);
        for (const key of Object.keys(configuration)) {
            compare(page["cfg_" + key], configuration[key]);
        }
        compare(form(page).twinFormLayouts.length, 1);
        compare(form(page).twinFormLayouts[0], parentForm);

        page.cfg_MouseForwardChanged.connect(function() {
            configuration.MouseForward = page.cfg_MouseForward;
        });
        page.cfg_MouseForward = true;
        compare(configuration.MouseForward, true);

        changedSpy.target = page;
        verify(changedSpy.valid);
        page.configurationChanged();
        compare(changedSpy.count, 1);
    }

    function test_withoutHostConfiguration() {
        const page = createTemporaryObject(configComponent, appearanceRoot, {
            parentLayout: null
        });
        verify(page !== null);
        compare(page.wallpaperConfiguration, null);
        compare(form(page).twinFormLayouts.length, 0);
    }

    function test_wallpaperConfigurationFallback() {
        wallpaper = { configuration: { MouseForward: true } };
        const page = createTemporaryObject(configComponent, appearanceRoot);
        verify(page !== null);
        compare(page.wallpaperConfiguration, wallpaper.configuration);
    }
}
