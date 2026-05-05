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
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

import {GnomeShellOverride} from './gnomeShellOverride.js';
import {LaunchSubprocess} from './launcher.js';

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
        this._spawnRenderer();
    }

    _spawnRenderer() {
        const overridePath = this._settings.get_string('renderer-binary');
        const argv = [];
        const rendererPath = overridePath && overridePath.length > 0
            ? overridePath
            : GLib.build_filenamev([this.path, 'renderer', 'renderer.js']);
        argv.push(rendererPath);
        argv.push('--instance-id', this._settings.get_string('instance-id'));
        argv.push('--display-name', this._settings.get_string('display-name'));

        this._currentProc = new LaunchSubprocess();
        this._currentProc.set_cwd(GLib.get_home_dir());
        this._currentProc.spawnv(argv);

        const proc = this._currentProc;
        proc.subprocess?.wait_async(null, (subprocess, res) => {
            try {
                subprocess.wait_finish(res);
            } catch (_e) { /* cancelled is fine */ }
            if (this._currentProc !== proc)
                return;
            this._currentProc = null;
            // Slow down respawn if exit was non-zero (likely crash).
            const cleanExit = subprocess.get_if_exited()
                && subprocess.get_exit_status() === 0;
            this._respawnDelayMs = cleanExit ? 100 : 1000;
            if (this._isEnabled)
                this._scheduleRespawn();
        });
    }

    _scheduleRespawn() {
        if (this._respawnId)
            return;
        this._respawnId = GLib.timeout_add(GLib.PRIORITY_DEFAULT,
            this._respawnDelayMs, () => {
                this._respawnId = 0;
                if (this._isEnabled)
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
        if (this._currentProc) {
            this._currentProc.cancellable?.cancel();
            this._currentProc.subprocess?.send_signal(15); // SIGTERM
            this._currentProc = null;
        }
        if (this._override) {
            this._override.disable();
            this._override = null;
        }
        this._settings = null;
    }
}
