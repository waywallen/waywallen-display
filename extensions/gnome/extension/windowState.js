// Reports per-monitor covering-window state to the daemon for autopause.
// The renderer windows are hidden and can't observe other windows, so
// the extension computes the WAYWALLEN_WIN_HAS_* bitmask per monitor and
// pipes it to the renderer over stdin (it routes by monitor geometry and
// calls set_window_state). The daemon owns all pause policy.

import Meta from 'gi://Meta';
import GLib from 'gi://GLib';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';

import * as Wallpaper from './wallpaper.js';
import {WindowExclusions} from './windowExclusions.js';

const APPLICATION_ID = Wallpaper.APPLICATION_ID;

// Mirrors WAYWALLEN_WIN_HAS_* in waywallen_display.h.
const WIN_NON_MINIMIZED = 1 << 0;
const WIN_ACTIVE        = 1 << 1;
const WIN_MAXIMIZED     = 1 << 2;
const WIN_FULLSCREEN    = 1 << 3;

const DEBOUNCE_MS = 80;

export class WindowStateMonitor {
    constructor(settings) {
        this._settings = settings;
        this._launcher = null;
        this._sigs = [];   // [obj, handlerId]
        this._lastFlags = new Map();  // monitor index -> last sent flags
        this._debounceId = 0;
        this._initId = 0;
        this._configs = new Map();
        this._windowSignals = new Map();
    }

    setLauncher(launcher) {
        this._launcher = launcher;
        // New renderer: force a resend of every monitor's state once its
        // display connection is up (set_window_state is dropped pre-connect).
        this._lastFlags.clear();
        this._configs.clear();
        if (this._initId)
            GLib.source_remove(this._initId);
        this._initId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, 1500, () => {
            this._initId = 0;
            this._recompute();
            return GLib.SOURCE_REMOVE;
        });
    }

    enable() {
        const queue = () => this._queue();
        const wm = global.window_manager;
        const disp = global.display;
        const wsm = global.workspace_manager;
        const overview = Main.overview;
        this._sigs.push([wm, wm.connect('size-change', queue)]);    // (un)maximize / fullscreen
        this._sigs.push([wm, wm.connect('minimize', queue)]);
        this._sigs.push([wm, wm.connect('unminimize', queue)]);
        this._sigs.push([wm, wm.connect_after('map', queue)]);
        this._sigs.push([wm, wm.connect('destroy', queue)]);
        this._sigs.push([disp, disp.connect('notify::focus-window', queue)]);
        this._sigs.push([disp, disp.connect('window-entered-monitor', queue)]);
        this._sigs.push([disp, disp.connect('window-left-monitor', queue)]);
        this._sigs.push([Main.layoutManager, Main.layoutManager.connect('monitors-changed', () => {
            this._lastFlags.clear();
            this._queue();
        })]);
        this._sigs.push([wsm, wsm.connect('active-workspace-changed', queue)]);
        this._sigs.push([wsm, wsm.connect('showing-desktop-changed', queue)]);
        this._sigs.push([this._settings, this._settings.connect('changed::overview-autopause', queue)]);
        this._sigs.push([overview, overview.connect('showing', queue)]);
        this._sigs.push([overview, overview.connect('hidden', queue)]);
        this._queue();
    }

    disable() {
        for (const [window, signals] of this._windowSignals) {
            for (const id of signals) { try { window.disconnect(id); } catch (_e) {} }
        }
        this._windowSignals.clear();
        this._configs.clear();
        for (const [obj, id] of this._sigs) {
            try { obj.disconnect(id); } catch (_e) {}
        }
        this._sigs = [];
        if (this._debounceId) {
            GLib.source_remove(this._debounceId);
            this._debounceId = 0;
        }
        if (this._initId) {
            GLib.source_remove(this._initId);
            this._initId = 0;
        }
        this._launcher = null;
        this._lastFlags.clear();
    }

    _queue() {
        if (this._debounceId)
            return;
        this._debounceId = GLib.timeout_add(GLib.PRIORITY_DEFAULT,
            DEBOUNCE_MS, () => {
                this._debounceId = 0;
                this._recompute();
                return GLib.SOURCE_REMOVE;
            });
    }

    applyConfig(geometry, config) {
        this._configs.set(`${geometry.x},${geometry.y}`, new WindowExclusions(config));
        this._lastFlags.clear();
        this._recompute();
    }

    resetConfig(geometry) {
        this._configs.delete(`${geometry.x},${geometry.y}`);
        this._lastFlags.clear();
    }

    _recompute() {
        if (!this._launcher?.running)
            return;
        const ws = global.workspace_manager.get_active_workspace();
        if (!ws)
            return;

        const windows = ws.list_windows();
        for (const [window, signals] of this._windowSignals) {
            if (windows.includes(window)) continue;
            for (const id of signals) { try { window.disconnect(id); } catch (_e) {} }
            this._windowSignals.delete(window);
        }
        const acc = new Map();  // monitor index -> flags
        for (const w of windows) {
            if (!this._windowSignals.has(w)) {
                const signals = ['notify::title', 'notify::wm-class', 'notify::gtk-application-id']
                    .map(signal => w.connect(signal, () => this._queue()));
                this._windowSignals.set(w, signals);
            }
            // Show-desktop hides normal windows without minimizing them.
            if ((Main.overview.visible && !this._settings.get_boolean('overview-autopause')) ||
                w.skip_taskbar || !w.showing_on_its_workspace())
                continue;
            if (w.title?.includes(APPLICATION_ID))
                continue;
            if (w.get_window_type?.() !== Meta.WindowType.NORMAL)
                continue;
            const m = w.get_monitor();
            if (m < 0)
                continue;
            const geometry = global.display.get_monitor_geometry(m);
            const config = this._configs.get(`${geometry.x},${geometry.y}`);
            const applicationId = w.get_gtk_application_id?.() || w.get_wm_class?.() || '';
            const title = w.get_title?.() || '';
            if (config?.matches(applicationId, title)) continue;
            let fl = (acc.get(m) ?? 0) | WIN_NON_MINIMIZED;
            if (w.has_focus())
                fl |= WIN_ACTIVE;
            if (w.fullscreen)
                fl |= WIN_FULLSCREEN;
            else if (this._isMaximized(w))
                fl |= WIN_MAXIMIZED;
            acc.set(m, fl);
        }

        const n = global.display.get_n_monitors();
        for (let m = 0; m < n; m++) {
            const fl = acc.get(m) ?? 0;
            if (this._lastFlags.get(m) === fl)
                continue;
            this._lastFlags.set(m, fl);
            const g = global.display.get_monitor_geometry(m);
            const config = this._configs.get(`${g.x},${g.y}`);
            const generation = config ? ` ${config.generation}` : '';
            this._launcher.writeStdin(`W ${g.x} ${g.y} ${fl}${generation}\n`);
        }
    }

    _isMaximized(w) {
        // is_maximized() is 49+; older versions use the maximized-* properties.
        if (typeof w.is_maximized === 'function')
            return w.is_maximized();
        return w.maximized_horizontally && w.maximized_vertically;
    }
}
