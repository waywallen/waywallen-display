import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

const BUS_NAME = 'org.waywallen.Display.Background';
const OBJECT_PATH = '/org/waywallen/Display/Background';
const PORTAL_NAME = 'org.freedesktop.portal.Desktop';
const PORTAL_PATH = '/org/freedesktop/portal/desktop';
const REQUEST_IFACE = 'org.freedesktop.portal.Request';
const XML = `<node><interface name="org.waywallen.Display.Background1">
    <property name="CanAutoUpdateStaticBackground" type="b" access="read"/>
    <method name="SetStaticBackground"><arg name="path" type="s" direction="in"/></method>
</interface></node>`;

Gio._promisify(Gio.File.prototype, 'query_info_async', 'query_info_finish');
Gio._promisify(Gio.File.prototype, 'read_async', 'read_finish');

export class SystemWallpaper {
    get CanAutoUpdateStaticBackground() {
        return true;
    }

    enable() {
        this._bus = Gio.DBus.session;
        this._exported = Gio.DBusExportedObject.wrapJSObject(XML, this);
        this._exported.export(this._bus, OBJECT_PATH);
        this._ownerId = Gio.bus_own_name_on_connection(this._bus, BUS_NAME,
            Gio.BusNameOwnerFlags.NONE, null, null);
    }

    SetStaticBackgroundAsync([path], invocation) {
        if (!GLib.path_is_absolute(path) || path.length > 4096 || path.includes('\0')) {
            invocation.return_dbus_error('org.freedesktop.DBus.Error.InvalidArgs',
                'Expected an absolute image path');
            return;
        }
        if (this._pending) {
            invocation.return_dbus_error('org.freedesktop.DBus.Error.Failed',
                'A system background update is already in progress');
            return;
        }
        const pending = {invocation, cancellable: new Gio.Cancellable()};
        this._pending = pending;
        pending.timeoutId = GLib.timeout_add_seconds(GLib.PRIORITY_DEFAULT, 120, () => {
            pending.timeoutId = 0;
            this._closeRequest();
            this._finish('System background update timed out');
            return GLib.SOURCE_REMOVE;
        });
        this._install(path, pending).catch(error => {
            if (this._pending === pending)
                this._finish(error.message);
        });
    }

    async _install(path, pending) {
        const file = Gio.File.new_for_path(path);
        const info = await file.query_info_async('standard::type', Gio.FileQueryInfoFlags.NONE,
            GLib.PRIORITY_DEFAULT, pending.cancellable);
        if (this._pending !== pending)
            return;
        if (info.get_file_type() !== Gio.FileType.REGULAR) {
            this._finish('Expected a regular image file', 'org.freedesktop.DBus.Error.InvalidArgs');
            return;
        }
        const image = await file.read_async(GLib.PRIORITY_DEFAULT, pending.cancellable);
        if (this._pending !== pending) {
            image.close_async(GLib.PRIORITY_DEFAULT, null, null);
            return;
        }
        pending.image = image;
        const fds = new Gio.UnixFDList();
        const fd = fds.append(image.get_fd());
        const token = `waywallen_${GLib.uuid_string_random().replaceAll('-', '')}`;
        const sender = this._bus.get_unique_name().slice(1).replaceAll('.', '_');
        const handle = `${PORTAL_PATH}/request/${sender}/${token}`;
        pending.handle = handle;
        // Subscribe before the call: a portal can complete before its method reply arrives.
        pending.signalId = this._bus.signal_subscribe(PORTAL_NAME, REQUEST_IFACE,
            'Response', handle, null, Gio.DBusSignalFlags.NONE,
            (_connection, _sender, _path, _interface, _signal, parameters) => {
                if (this._pending !== pending)
                    return;
                const [response] = parameters.deep_unpack();
                this._finish(response === 0 ? null :
                    response === 1 ? 'Wallpaper portal request was cancelled' : 'Wallpaper portal request failed');
            });
        this._bus.call_with_unix_fd_list(PORTAL_NAME, PORTAL_PATH,
            'org.freedesktop.portal.Wallpaper', 'SetWallpaperFile',
            new GLib.Variant('(sha{sv})', ['', fd, {
                'set-on': new GLib.Variant('s', 'background'),
                'show-preview': new GLib.Variant('b', false),
                'handle_token': new GLib.Variant('s', token),
            }]), new GLib.VariantType('(o)'), Gio.DBusCallFlags.NONE, -1, fds,
            pending.cancellable, (connection, result) => {
                try {
                    const [reply] = connection.call_with_unix_fd_list_finish(result);
                    if (this._pending === pending && reply.deep_unpack()[0] !== handle)
                        this._finish('Wallpaper portal returned an unexpected request handle');
                } catch (error) {
                    if (this._pending === pending)
                        this._finish(error.message);
                }
            });
    }

    _closeRequest() {
        if (!this._pending?.handle)
            return;
        this._bus.call(PORTAL_NAME, this._pending.handle, REQUEST_IFACE, 'Close',
            null, null, Gio.DBusCallFlags.NONE, 5000, null, (bus, result) => {
                try { bus.call_finish(result); } catch (_error) { /* The request may already be closed. */ }
            });
    }

    _finish(error, errorName = 'org.freedesktop.DBus.Error.Failed') {
        const pending = this._pending;
        if (!pending)
            return;
        this._pending = null;
        pending.cancellable.cancel();
        if (pending.signalId)
            this._bus.signal_unsubscribe(pending.signalId);
        if (pending.timeoutId)
            GLib.source_remove(pending.timeoutId);
        pending.image?.close_async(GLib.PRIORITY_DEFAULT, null, null);
        if (error)
            pending.invocation.return_dbus_error(errorName, error);
        else
            pending.invocation.return_value(new GLib.Variant('()', []));
    }

    disable() {
        this._closeRequest();
        this._finish('Desktop integration was disabled');
        if (this._ownerId)
            Gio.bus_unown_name(this._ownerId);
        this._ownerId = 0;
        this._exported?.unexport();
        this._exported = null;
    }
}
