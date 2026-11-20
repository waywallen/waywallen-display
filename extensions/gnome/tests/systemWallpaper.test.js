// Run in a private bus: dbus-run-session gjs -m extensions/gnome/tests/systemWallpaper.test.js
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import System from 'system';
import {SystemWallpaper} from '../extension/systemWallpaper.js';

function assert(value, message = 'assertion failed') {
    if (!value)
        throw new Error(message);
}

function call(name, path, iface, method, parameters) {
    return new Promise((resolve, reject) => {
        Gio.DBus.session.call(name, path, iface, method, parameters, null,
            Gio.DBusCallFlags.NONE, 5000, null, (bus, result) => {
                try { resolve(bus.call_finish(result)); } catch (error) { reject(error); }
            });
    });
}

const bus = Gio.DBus.session;
const serviceName = 'org.waywallen.Display.Background';
const servicePath = '/org/waywallen/Display/Background';
const serviceInterface = 'org.waywallen.Display.Background1';
const portalPath = '/org/freedesktop/portal/desktop';
const loop = new GLib.MainLoop(null, false);
let failed = false;

async function run() {
    let response = 0;
    let calls = 0;
    const portal = Gio.DBusExportedObject.wrapJSObject(`<node>
        <interface name="org.freedesktop.portal.Wallpaper">
            <method name="SetWallpaperFile">
                <arg type="s" direction="in"/><arg type="h" direction="in"/>
                <arg type="a{sv}" direction="in"/><arg type="o" direction="out"/>
            </method>
        </interface></node>`, {
        SetWallpaperFileAsync([parent, fd, options], invocation) {
            calls++;
            assert(parent === '');
            assert(options['set-on'].deep_unpack() === 'background');
            assert(options['show-preview'].deep_unpack() === false);
            const fdList = invocation.get_message().get_unix_fd_list();
            const imageFd = fdList.get(fd);
            const [read, bytes] = GLib.file_get_contents(`/proc/self/fd/${imageFd}`);
            GLib.close(imageFd);
            assert(read && bytes.length > 0, 'image fd was not transferred');
            const sender = invocation.get_sender().slice(1).replaceAll('.', '_');
            const handle = `${portalPath}/request/${sender}/${options.handle_token.deep_unpack()}`;
            // Exercise Response arriving before the method reply.
            bus.emit_signal(null, handle, 'org.freedesktop.portal.Request', 'Response',
                new GLib.Variant('(ua{sv})', [response, {}]));
            invocation.return_value(new GLib.Variant('(o)', [handle]));
        },
    });
    const portalOwner = await new Promise(resolve => {
        const id = Gio.bus_own_name_on_connection(bus, 'org.freedesktop.portal.Desktop',
            Gio.BusNameOwnerFlags.NONE, () => {
                portal.export(bus, portalPath);
                resolve(id);
            }, () => {});
    });
    const service = new SystemWallpaper();
    service.enable();
    for (let i = 0; i < 100; i++) {
        const owner = await call('org.freedesktop.DBus', '/org/freedesktop/DBus',
            'org.freedesktop.DBus', 'NameHasOwner', new GLib.Variant('(s)', [serviceName]));
        if (owner.deep_unpack()[0])
            break;
        await new Promise(resolve => GLib.timeout_add(GLib.PRIORITY_DEFAULT, 10, () => {
            resolve(); return GLib.SOURCE_REMOVE;
        }));
    }
    const [fd, image] = GLib.file_open_tmp('waywallen-background-test-XXXXXX');
    GLib.close(fd);
    GLib.file_set_contents(image, 'test image');
    try {
        const capability = await call(serviceName, servicePath,
            'org.freedesktop.DBus.Properties', 'Get',
            new GLib.Variant('(ss)', [serviceInterface, 'CanAutoUpdateStaticBackground']));
        assert(capability.deep_unpack()[0].deep_unpack() === true,
            'safe automatic updates were not advertised');
        const update = path => call(serviceName, servicePath, serviceInterface,
            'SetStaticBackground', new GLib.Variant('(s)', [path]));
        await update(image);
        assert(calls === 1);
        let rejected = false;
        try { await update('relative.png'); } catch (_error) { rejected = true; }
        assert(rejected && calls === 1, 'relative path reached the portal');
        rejected = false;
        try { await update('/dev/null'); } catch (error) { rejected = error.message.includes('regular image file'); }
        assert(rejected && calls === 1, 'non-regular file reached the portal');
        response = 1;
        rejected = false;
        try { await update(image); } catch (error) { rejected = error.message.includes('cancelled'); }
        assert(rejected, 'portal cancellation was reported as success');
        service.disable();
        rejected = false;
        try { await update(image); } catch (_error) { rejected = true; }
        assert(rejected, 'disabled service was still callable');
    } finally {
        service.disable();
        portal.unexport();
        Gio.bus_unown_name(portalOwner);
        GLib.unlink(image);
    }
    print('GNOME system background: fd transfer, early response, validation, cancellation and lifecycle passed');
}

run().catch(error => { failed = true; printerr(error.stack); }).finally(() => loop.quit());
loop.run();
if (failed)
    System.exit(1);
