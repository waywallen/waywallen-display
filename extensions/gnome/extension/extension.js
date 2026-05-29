// Top-level GNOME Shell extension for org.waywallen.gnome.
//
// Lifecycle:
//   enable()  — install monkey-patches, generate instance-id if needed,
//               spawn renderer, hook background-actor injection.
//   disable() — undo patches, terminate renderer, clean up.
//
// Multiple instances of the renderer are NOT spawned — one process
// handles all monitors via per-monitor windows.

import GLib from 'gi://GLib';
import Gio from 'gi://Gio';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

import {GnomeShellOverride} from './gnomeShellOverride.js';
import {LaunchSubprocess} from './launcher.js';
import {WindowManager} from './windowManager.js';
import {PointerForwarder} from './pointerForwarder.js';

const DAEMON_BUS_NAME  = 'org.waywallen.waywallen.Daemon';
const DAEMON_OBJ_PATH  = '/org/waywallen/waywallen/Daemon';
const DAEMON_IFACE     = 'org.waywallen.waywallen.Daemon1';
const DAEMON_READY_SIG = 'Ready';

function generateUuidV4() {
    return 'xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx'.replace(/[xy]/g, c => {
        const r = Math.floor(Math.random() * 16);
        const v = c === 'x' ? r : (r & 0x3) | 0x8;
        return v.toString(16);
    });
}

export default class WaywallenExtension extends Extension {
    constructor(metadata) {
        super(metadata);
        this._isEnabled = false;
        this._currentProc = null;
        this._respawnId = 0;
        this._respawnDelayMs = 100;
        // Bus state — set by the bus watcher; reset on disable.
        this._daemonUp = false;
        this._busWatchId = 0;
        this._readySigId = 0;
    }

    enable() {
        this._settings = this.getSettings();

        if (!this._settings.get_string('instance-id')) {
            this._settings.set_string('instance-id', generateUuidV4());
            // Persist immediately; gschema flushes lazily otherwise.
            this._settings.apply();
        }

        this._override = new GnomeShellOverride();

        // Defer renderer spawn + override install until shell startup
        // animation is past, so we don't fight Main.layoutManager's
        // initial background pass.
        if (Main.layoutManager._startingUp) {
            this._startupSignalId = Main.layoutManager.connect(
                'startup-complete', () => this._innerEnable());
        } else {
            GLib.idle_add(GLib.PRIORITY_DEFAULT, () => {
                this._innerEnable();
                return GLib.SOURCE_REMOVE;
            });
        }
        this._isEnabled = true;
    }

    _innerEnable() {
        if (this._startupSignalId) {
            Main.layoutManager.disconnect(this._startupSignalId);
            this._startupSignalId = 0;
        }
        if (!this._isEnabled)
            return;
        this._override.enable();
        this._windowMgr = new WindowManager();
        this._windowMgr.enable();
        this._pointer = new PointerForwarder();
        this._pointer.enable();
        this._watchDaemon();
    }

    _watchDaemon() {
        const bus = Gio.DBus.session;
        this._busWatchId = Gio.bus_watch_name(
            Gio.BusType.SESSION, DAEMON_BUS_NAME,
            Gio.BusNameWatcherFlags.NONE,
            () => this._onDaemonAppeared(),
            () => this._onDaemonVanished());
        this._readySigId = bus.signal_subscribe(
            DAEMON_BUS_NAME, DAEMON_IFACE, DAEMON_READY_SIG, DAEMON_OBJ_PATH,
            null, Gio.DBusSignalFlags.NONE,
            () => this._onDaemonReady());
    }

    _onDaemonAppeared() {
        log('[waywallen] daemon appeared on session bus');
        this._daemonUp = true;
        this._respawnDelayMs = 100;
        if (this._isEnabled && !this._currentProc)
            this._spawnRenderer();
    }

    _onDaemonVanished() {
        log('[waywallen] daemon vanished from session bus');
        this._daemonUp = false;
        if (this._respawnId) {
            GLib.source_remove(this._respawnId);
            this._respawnId = 0;
        }
        if (this._currentProc) {
            this._currentProc.cancellable?.cancel();
            this._currentProc.subprocess?.send_signal(15);
            this._currentProc = null;
        }
    }

    _onDaemonReady() {
        log('[waywallen] daemon Ready signal');
        this._daemonUp = true;
        if (this._isEnabled && !this._currentProc)
            this._spawnRenderer();
    }

    _spawnRenderer() {
        if (!this._daemonUp)
            return;

        const overridePath = this._settings.get_string('renderer-binary');
        const rendererPath = overridePath && overridePath.length > 0
            ? overridePath
            : GLib.build_filenamev([this.path, 'renderer', 'renderer.js']);
        const argv = [
            rendererPath,
            '--instance-id',   this._settings.get_string('instance-id'),
            '--display-name',  this._settings.get_string('display-name'),
        ];

        this._currentProc = new LaunchSubprocess();
        this._currentProc.set_cwd(GLib.get_home_dir());

        const bundleLib     = GLib.build_filenamev([this.path, 'lib']);
        const bundleTypelib = GLib.build_filenamev([bundleLib, 'girepository-1.0']);
        if (GLib.file_test(bundleTypelib, GLib.FileTest.IS_DIR))
            this._currentProc.prepend_env('GI_TYPELIB_PATH', bundleTypelib);
        if (GLib.file_test(bundleLib, GLib.FileTest.IS_DIR))
            this._currentProc.prepend_env('LD_LIBRARY_PATH', bundleLib);
        this._currentProc.set_env('GTK_A11Y', 'none');

        this._currentProc.spawnv(argv);
        this._windowMgr?.setLauncher(this._currentProc);
        this._pointer?.setLauncher(this._currentProc);

        const proc = this._currentProc;
        proc.subprocess?.wait_async(null, (subprocess, res) => {
            try {
                subprocess.wait_finish(res);
            } catch (_e) { /* cancelled is fine */ }
            if (this._currentProc !== proc)
                return;
            this._currentProc = null;
            if (!this._isEnabled || !this._daemonUp)
                return;
            // Daemon is up but renderer died: exponential backoff up to 30s.
            const cleanExit = subprocess.get_if_exited()
                && subprocess.get_exit_status() === 0;
            this._respawnDelayMs = cleanExit
                ? 100
                : Math.min(this._respawnDelayMs * 2, 30000);
            this._scheduleRespawn();
        });
    }

    _scheduleRespawn() {
        if (this._respawnId)
            return;
        this._respawnId = GLib.timeout_add(GLib.PRIORITY_DEFAULT,
            this._respawnDelayMs, () => {
                this._respawnId = 0;
                if (this._isEnabled && this._daemonUp)
                    this._spawnRenderer();
                return GLib.SOURCE_REMOVE;
            });
    }

    disable() {
        this._isEnabled = false;
        if (this._startupSignalId) {
            Main.layoutManager.disconnect(this._startupSignalId);
            this._startupSignalId = 0;
        }
        if (this._respawnId) {
            GLib.source_remove(this._respawnId);
            this._respawnId = 0;
        }
        if (this._busWatchId) {
            Gio.bus_unwatch_name(this._busWatchId);
            this._busWatchId = 0;
        }
        if (this._readySigId) {
            Gio.DBus.session.signal_unsubscribe(this._readySigId);
            this._readySigId = 0;
        }
        this._daemonUp = false;
        if (this._currentProc) {
            this._currentProc.cancellable?.cancel();
            this._currentProc.subprocess?.send_signal(15);
            this._currentProc = null;
        }
        if (this._override) {
            this._override.disable();
            this._override = null;
        }
        if (this._windowMgr) {
            this._windowMgr.disable();
            this._windowMgr = null;
        }
        if (this._pointer) {
            this._pointer.disable();
            this._pointer = null;
        }
        this._settings = null;
    }
}
