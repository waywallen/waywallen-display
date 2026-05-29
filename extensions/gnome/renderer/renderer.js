#!/usr/bin/env -S gjs -m
//
// One Gtk.ApplicationWindow per Gdk.Monitor. The wallpaper DMA-BUF is
// imported and presented through Waywallen.ShadowPaintable (the C
// wrapper owns the GdkTexture lifetime — doing it in JS leaked because
// GJS GC is lazy and GSK never evicted the cached VkImages).

import GLib from 'gi://GLib';
import Gio from 'gi://Gio';
import Gdk from 'gi://Gdk?version=4.0';
import Gtk from 'gi://Gtk?version=4.0';
import Waywallen from 'gi://Waywallen?version=1.0';
import cairo from 'cairo';
import system from 'system';

// GLib.unix_fd_source_new / Gio.UnixInputStream moved to the GLibUnix /
// GioUnix namespaces in gjs 1.86+.
let GLibUnix = null;
try { GLibUnix = imports.gi.GLibUnix; } catch (_e) {}
let GioUnix = null;
try { GioUnix = imports.gi.GioUnix; } catch (_e) {}

const APP_ID = 'io.github.waywallen.WaywallenRenderer';

// WwHandshakeResult / WwHandshakeState values.
const HS_DONE = 1;
const HS_NEED_WRITE = 3;
const HS_READY = 6;

function logIndexed(idx, msg) {
    printerr(`[ww-renderer ${idx}] ${msg}`);
}

function addFdWatch(fd, condition, callback) {
    const newSource = GLibUnix?.fd_source_new ?? GLib.unix_fd_source_new;
    const source = newSource(fd, condition);
    source.set_callback(callback);
    return source.attach(null);
}

// Stable per-monitor suffix for the daemon's instance-id / display-name,
// so each physical monitor gets its own per-display settings slot. Prefer
// the connector name (DP-1, HDMI-A-1, …); fall back to manufacturer+model,
// then the enumeration index. Survives reorder/replug like the KDE plugin
// (identical monitors swapped between connectors swap wallpapers — same
// caveat as KDE).
function monitorKey(monitor, index) {
    const conn = monitor.get_connector?.();
    if (conn)
        return conn;
    const mfg = monitor.get_manufacturer?.() ?? '';
    const model = monitor.get_model?.() ?? '';
    if (mfg || model)
        return `${mfg}-${model}`;
    return `mon${index}`;
}

function parseArgs(argv) {
    const opts = {
        instanceId: '',
        displayName: 'gnome-shell',
        socketPath: null,
    };
    for (let i = 0; i < argv.length; i++) {
        const a = argv[i];
        if (a === '--instance-id' && i + 1 < argv.length) {
            opts.instanceId = argv[++i];
        } else if (a === '--display-name' && i + 1 < argv.length) {
            opts.displayName = argv[++i];
        } else if (a === '--socket' && i + 1 < argv.length) {
            opts.socketPath = argv[++i];
        }
    }
    return opts;
}

class MonitorRenderer {
    constructor(monitor, monitorIndex, opts) {
        this._monitor = monitor;
        this._index = monitorIndex;
        this._opts = opts;

        // Per-monitor identity: base id/name + stable monitor key, so the
        // daemon keys settings per physical monitor instead of collapsing
        // every output onto one display.
        const key = monitorKey(monitor, monitorIndex);
        this._instanceId = opts.instanceId ? `${opts.instanceId}:${key}` : '';
        this._displayName = `${opts.displayName}:${key}`;

        this._window = null;
        this._picture = null;
        this._display = null;

        this._inSourceId = 0;
        this._outSourceId = 0;
        this._paintable = null;

        this._destroyed = false;
    }

    build(app) {
        this._window = new Gtk.ApplicationWindow({
            application: app,
            decorated: false,
            resizable: false,
        });

        // Title hint consumed by windowManager.js: keepMinimized keeps the
        // actor off the stage (so it doesn't cover the panel) while the
        // wl_surface stays live for Clutter.Clone to mirror.
        const titleHint = JSON.stringify({
            keepMinimized: true,
            keepAtBottom: true,
            keepPosition: true,
        });
        this._window.set_title(`@${APP_ID}!${titleHint}|${this._index}`);

        const geom = this._monitor.get_geometry();
        // Pin the size: a minimized Wayland window gets no configure with a
        // real allocation, so without an explicit request Gtk.Picture
        // measures height as Infinity and gsk commits a degenerate buffer.
        this._picture = new Gtk.Picture({
            can_shrink: true,
            content_fit: Gtk.ContentFit.FILL,
            width_request: geom.width,
            height_request: geom.height,
        });
        this._window.set_child(this._picture);
        this._window.set_default_size(geom.width, geom.height);

        this._window.connect('realize', () => this._onRealize());
        this._window.fullscreen_on_monitor(this._monitor);
        this._window.present();
    }

    _onRealize() {
        try {
            // Disable input two ways: widget-level can_target stops GTK
            // dispatching clicks, empty wl_surface input region stops
            // mutter's implicit pointer grab from locking the user out.
            this._window.set_can_target(false);
            this._window.set_can_focus(false);
            if (this._picture) {
                this._picture.set_can_target(false);
                this._picture.set_can_focus(false);
            }
            const surface = this._window.get_surface();
            if (surface?.set_input_region) {
                try {
                    surface.set_input_region(new cairo.Region());
                } catch (e) {
                    logIndexed(this._index, `set_input_region failed: ${e}`);
                }
            }
            this._initWaywallen();
        } catch (e) {
            logIndexed(this._index, `init failed: ${e}`);
            this._exit();
        }
    }

    _initWaywallen() {
        logIndexed(this._index, `display=${this._displayName} id=${this._instanceId}`);
        const d = Waywallen.Display.new();
        if (!d.bind_dmabuf_relay())
            throw new Error('bind_dmabuf_relay failed');
        this._display = d;

        d.connect('textures-ready',
            (_o, count, w, h, fourcc, modifier, backend) =>
                this._onTexturesReady(count, w, h, fourcc, modifier, backend));
        d.connect('textures-releasing', () => this._onTexturesReleasing());
        d.connect('config',
            (_o, sx, sy, sw, sh, dx, dy, dw, dh, transform, cr, cg, cb, ca) =>
                this._paintable?.set_config(sx, sy, sw, sh, dx, dy, dw, dh,
                                            transform, cr, cg, cb, ca));
        d.connect('frame-ready',
            (_o, idx, seq, fd) => this._onFrameReady(idx, seq, fd));
        d.connect('disconnected',
            (_o, code, msg) => this._onDisconnected(code, msg));

        const geom = this._monitor.get_geometry();
        const refreshMhz = this._monitor.get_refresh_rate() || 60000;
        if (!d.begin_connect(this._opts.socketPath,
                             this._displayName,
                             this._instanceId,
                             geom.width, geom.height, refreshMhz))
            throw new Error('begin_connect failed');

        const fd = d.get_fd();
        if (fd < 0)
            throw new Error('display fd is -1 after begin_connect');

        // Watch readability always; _setOutWatch toggles the OUT watch
        // when the handshake needs to write.
        this._inSourceId = addFdWatch(fd, GLib.IOCondition.IN,
                                      () => this._driveIO());
        this._driveIO();
    }

    _setOutWatch(want) {
        const fd = this._display ? this._display.get_fd() : -1;
        if (want && this._outSourceId === 0 && fd >= 0) {
            this._outSourceId = addFdWatch(fd, GLib.IOCondition.OUT,
                                           () => this._driveIO());
        } else if (!want && this._outSourceId !== 0) {
            GLib.source_remove(this._outSourceId);
            this._outSourceId = 0;
        }
    }

    _driveIO() {
        if (this._destroyed || !this._display)
            return GLib.SOURCE_REMOVE;
        if (this._display.handshake_state() !== HS_READY) {
            const r = this._display.advance_handshake();
            if (r < 0) {
                logIndexed(this._index, `advance_handshake error: ${r}`);
                return GLib.SOURCE_REMOVE;
            }
            this._setOutWatch(r === HS_NEED_WRITE);
            // Only DONE means we reached READY this round; otherwise keep
            // polling. Fall through to dispatch on DONE.
            if (r !== HS_DONE)
                return GLib.SOURCE_CONTINUE;
            this._setOutWatch(false);
        }
        const dr = this._display.dispatch();
        if (dr < 0) {
            logIndexed(this._index, `dispatch error: ${dr}`);
            return GLib.SOURCE_REMOVE;
        }
        return GLib.SOURCE_CONTINUE;
    }

    _onTexturesReady(count, w, h, fourcc, _modifier, backend) {
        const [ok, sfd, nPlanes, strides, offsets, smod] =
            this._display.get_shadow_export();
        if (!ok) {
            logIndexed(this._index, 'get_shadow_export failed');
            return;
        }
        if (!this._paintable) {
            this._paintable = Waywallen.ShadowPaintable.new();
            this._picture.set_paintable(this._paintable);
        }
        // set_shadow takes ownership of the fd.
        this._paintable.set_shadow(sfd, nPlanes, w, h, fourcc, smod,
                                   strides, offsets);
        logIndexed(this._index,
            `bound shadow ${w}x${h} fourcc=0x${fourcc.toString(16)} ` +
            `count=${count} backend=${backend}`);
    }

    _onTexturesReleasing() {
        this._paintable?.clear();
    }

    _onFrameReady(_idx, _seq, fd) {
        if (fd >= 0)
            Waywallen.Display.close_fd(fd);
        this._paintable?.refresh();
    }

    monitorGeom() {
        return this._monitor.get_geometry();
    }

    sendPointerMotion(x, y, ts) {
        this._display?.send_pointer_motion(x, y, ts, 0);
    }

    sendPointerButton(x, y, code, pressed, ts) {
        this._display?.send_pointer_button(x, y, code, pressed, ts, 0);
    }

    sendPointerAxis(x, y, dx, dy, ts) {
        this._display?.send_pointer_axis(x, y, dx, dy, ts, 0);
    }

    _onDisconnected(code, msg) {
        logIndexed(this._index, `disconnected: code=${code} msg=${msg}`);
        this._exit();  // extension respawns us
    }

    _exit() {
        if (this._destroyed)
            return;
        this._destroyed = true;
        Gtk.Application.get_default()?.quit();
    }

    destroy() {
        if (this._destroyed)
            return;
        this._destroyed = true;
        if (this._inSourceId) {
            GLib.source_remove(this._inSourceId);
            this._inSourceId = 0;
        }
        if (this._outSourceId) {
            GLib.source_remove(this._outSourceId);
            this._outSourceId = 0;
        }
        if (this._picture)
            this._picture.set_paintable(null);
        if (this._paintable) {
            this._paintable.clear();
            this._paintable = null;
        }
        if (this._display) {
            this._display.disconnect();
            this._display = null;
        }
        if (this._window) {
            this._window.destroy();
            this._window = null;
        }
    }
}

const opts = parseArgs(ARGV);

const app = new Gtk.Application({
    application_id: APP_ID,
    flags: Gio.ApplicationFlags.NON_UNIQUE,
});

let renderers = [];

// Pointer events forwarded by the extension over stdin, in global
// compositor pixel coords (the extension can't deliver them to our
// hidden window, so it captures and pipes them here). Dispatch to the
// MonitorRenderer whose monitor geometry contains the point, converting
// to monitor-local coords. Line formats:
//   M gx gy ts                       motion
//   B gx gy code pressed ts          button (pressed: 1/0)
//   A gx gy dx dy ts                 axis (wheel notches)
function dispatchPointer(line) {
    const f = line.split(' ');
    const gx = parseFloat(f[1]);
    const gy = parseFloat(f[2]);
    if (!Number.isFinite(gx) || !Number.isFinite(gy))
        return;
    const r = renderers.find(rr => {
        const g = rr.monitorGeom();
        return gx >= g.x && gx < g.x + g.width &&
               gy >= g.y && gy < g.y + g.height;
    });
    if (!r)
        return;
    const g = r.monitorGeom();
    const lx = gx - g.x;
    const ly = gy - g.y;
    switch (f[0]) {
    case 'M': r.sendPointerMotion(lx, ly, parseInt(f[3]) || 0); break;
    case 'B': r.sendPointerButton(lx, ly, parseInt(f[3]) || 0,
                                  f[4] === '1', parseInt(f[5]) || 0); break;
    case 'A': r.sendPointerAxis(lx, ly, parseFloat(f[3]) || 0,
                                parseFloat(f[4]) || 0, parseInt(f[5]) || 0); break;
    }
}

function readPointerStdin() {
    const UnixInputStream = GioUnix?.InputStream ?? Gio.UnixInputStream;
    const stdin = Gio.DataInputStream.new(
        UnixInputStream.new(0, false));
    const loop = () => {
        stdin.read_line_async(GLib.PRIORITY_DEFAULT, null, (s, res) => {
            let line, len;
            try {
                [line, len] = s.read_line_finish_utf8(res);
            } catch (_e) {
                return;
            }
            if (len > 0 && line)
                dispatchPointer(line);
            if (line !== null)
                loop();
        });
    };
    loop();
}

app.connect('activate', () => {
    const display = Gdk.Display.get_default();
    const monitors = display.get_monitors();
    const n = monitors.get_n_items();
    if (n === 0) {
        printerr('[ww-renderer] no monitors; exiting');
        app.quit();
        return;
    }
    for (let i = 0; i < n; i++) {
        const mon = monitors.get_item(i);
        const r = new MonitorRenderer(mon, i, opts);
        r.build(app);
        renderers.push(r);
    }
    readPointerStdin();
});

app.connect('shutdown', () => {
    for (const r of renderers)
        r.destroy();
    renderers = [];
});

const status = app.run([system.programInvocationName]);
system.exit(status);
