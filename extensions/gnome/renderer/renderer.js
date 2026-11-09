#!/usr/bin/env -S gjs -m
// One Gtk.ApplicationWindow per Gdk.Monitor; the wallpaper DMA-BUF is
// presented through Waywallen.PresentationWidget (C-side texture lifetime
// and toolkit submission state).

import GLib from 'gi://GLib';
import Gio from 'gi://Gio';
import Gdk from 'gi://Gdk?version=4.0';
import Gtk from 'gi://Gtk?version=4.0';
import Waywallen from 'gi://Waywallen?version=1.0';
import Gettext from 'gettext';
import cairo from 'cairo';
import system from 'system';

import {encodeControlFrame} from '../controlCodec.js';

const {vprintf} = imports.format;

// GLib.unix_fd_source_new / Gio.UnixInputStream moved to the GLibUnix /
// GioUnix namespaces in gjs 1.86+.
let GLibUnix = null;
try { GLibUnix = imports.gi.GLibUnix; } catch (_e) {}
let GioUnix = null;
try { GioUnix = imports.gi.GioUnix; } catch (_e) {}

const APP_ID = 'io.github.waywallen.WaywallenRenderer';
const GETTEXT_DOMAIN = 'waywallen-gnome';
const localeDir = GLib.getenv('WAYWALLEN_LOCALEDIR');
if (localeDir)
    Gettext.bindtextdomain(GETTEXT_DOMAIN, localeDir);
const _ = message => Gettext.dgettext(GETTEXT_DOMAIN, message);

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

// Stable per-monitor key for the daemon's per-display settings: connector
// name, else manufacturer+model, else index.
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

function monitorDisplayName(monitor) {
    const model = (monitor.get_model?.() ?? '').trim();
    if (model)
        return model;
    const connector = (monitor.get_connector?.() ?? '').trim();
    if (connector)
        return connector;
    return 'gnome-shell';
}

let _diagCssDone = false;
function ensureDiagCss() {
    if (_diagCssDone)
        return;
    _diagCssDone = true;
    const css = new Gtk.CssProvider();
    css.load_from_string(
        '.ww-diag { background: rgba(0,0,0,0.55); color: #cdd6f4; ' +
        'font-family: monospace; font-size: 13px; padding: 6px 8px; ' +
        'border-radius: 6px; }');
    Gtk.StyleContext.add_provider_for_display(
        Gdk.Display.get_default(), css,
        Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION);
}

function parseArgs(argv) {
    const opts = {
        instanceId: '',
        displayName: '',
        socketPath: null,
        diagnostics: false,
    };
    for (let i = 0; i < argv.length; i++) {
        const a = argv[i];
        if (a === '--instance-id' && i + 1 < argv.length) {
            opts.instanceId = argv[++i];
        } else if (a === '--display-name' && i + 1 < argv.length) {
            opts.displayName = argv[++i];
        } else if (a === '--socket' && i + 1 < argv.length) {
            opts.socketPath = argv[++i];
        } else if (a === '--diagnostics') {
            opts.diagnostics = true;
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
        this._displayName = opts.displayName.trim() || monitorDisplayName(monitor);

        this._window = null;
        this._presentationWidget = null;
        this._display = null;

        this._inSourceId = 0;
        this._outSourceId = 0;

        this._destroyed = false;
        this._presentation = null;
        this._presentationReady = false;
        this._pointerMotion = null;
    }

    build(app) {
        this._window = new Gtk.ApplicationWindow({
            application: app,
            decorated: false,
            resizable: false,
        });

        const geom = this._monitor.get_geometry();
        // position must be this monitor's origin: windowManager pins the
        // window there, else every monitor's window collapses onto (0,0) and
        // get_monitor() stops being unique for per-monitor matching.
        this._updateWindowTitle();

        // Pin the size: a minimized Wayland window gets no configure, so the
        // source widget needs an explicit logical allocation.
        this._presentationWidget = Waywallen.PresentationWidget.new();
        this._presentationWidget.set_size_request(geom.width, geom.height);

        if (this._opts.diagnostics)
            this._window.set_child(this._buildDiagOverlay());
        else
            this._window.set_child(this._presentationWidget);
        this._window.set_default_size(geom.width, geom.height);

        this._window.connect('realize', () => this._onRealize());
        // Do not fullscreen the source window. The visible wallpaper is a
        // Clutter.Clone of this window injected into the desktop background
        // actor, so the window itself is only a DMA-BUF source surface. The
        // correct size is already set via width_request/height_request and
        // set_default_size(), while the native rendering resolution is
        // negotiated separately (geom * scale). Fullscreening here causes
        // GNOME Shell to hide the top panel on an empty desktop.
        // this._window.fullscreen_on_monitor(this._monitor);
        this._window.present();
    }

    _buildDiagOverlay() {
        ensureDiagCss();
        const overlay = new Gtk.Overlay();
        overlay.set_child(this._presentationWidget);
        this._diagLabel = new Gtk.Label({
            halign: Gtk.Align.START,
            valign: Gtk.Align.START,
            margin_top: 12,
            margin_start: 12,
            xalign: 0,
        });
        this._diagLabel.add_css_class('ww-diag');
        overlay.add_overlay(this._diagLabel);
        this._diagTimerId = GLib.timeout_add_seconds(GLib.PRIORITY_DEFAULT, 1,
            () => { this._updateDiag(); return GLib.SOURCE_CONTINUE; });
        return overlay;
    }

    _updateDiag() {
        if (!this._diagLabel)
            return;
        const now = GLib.get_monotonic_time();
        const dt = this._diagLast ? (now - this._diagLast) / 1e6 : 0;
        const frames = this._frames ?? 0;
        const fps = dt > 0 ? ((frames - (this._diagFrames ?? 0)) / dt) : 0;
        this._diagLast = now;
        this._diagFrames = frames;
        const f = this._winFlags ?? 0;
        const ws = `${f & 1 ? _('win') + ' ' : ''}${f & 2 ? _('active') + ' ' : ''}` +
                   `${f & 4 ? _('max') + ' ' : ''}${f & 8 ? _('full') : ''}`.trim() || _('none');
        this._diagLabel.set_text([
            vprintf(_('mon %d  %s'), [this._index, this._displayName]),
            vprintf(_('id %s'), [this._instanceId || '—']),
            vprintf(_('disp %dx%d scale=%s'), [this._pw ?? '?', this._ph ?? '?', this._scale ?? '?']),
            vprintf(_('tex %dx%d fourcc=0x%s backend=%s'), [
                this._texW ?? '?', this._texH ?? '?', (this._fourcc ?? 0).toString(16), this._backend ?? '?']),
            vprintf(_('fps %s  frames %d'), [fps.toFixed(1), frames]),
            vprintf(_('winstate 0x%s (%s)'), [f.toString(16), ws]),
            `pause-effect cfg=${this._presentation?.config.generation ?? 0} ` +
            `dyn=${this._presentation?.state.generation ?? 0} ` +
            `kind=${this._presentation?.config.pauseEffect.kind ?? 0} ` +
            `active=${this._presentation?.state.pauseEffect.active ?? false} ` +
            `radius=${this._presentation?.config.pauseEffect.blur.radius ?? 30}`,
        ].join('\n'));
    }

    _onRealize() {
        try {
            // can_target + empty input region: stop GTK clicks and mutter's
            // implicit pointer grab from locking the user out.
            this._window.set_can_target(false);
            this._window.set_can_focus(false);
            if (this._presentationWidget) {
                this._presentationWidget.set_can_target(false);
                this._presentationWidget.set_can_focus(false);
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
        let followGpu = true;
        if (this._preferredGpu) {
            const {major, minor} = this._preferredGpu;
            if (!d.bind_dmabuf_relay_on_device(major, minor)) {
                logIndexed(this._index, `GPU ${major}:${minor} unavailable; using default relay GPU`);
                if (!d.bind_dmabuf_relay())
                    throw new Error('bind_dmabuf_relay failed');
                followGpu = false;
            }
        } else if (!d.bind_dmabuf_relay()) {
            throw new Error('bind_dmabuf_relay failed');
        }
        if (!d.set_follow_producer_gpu(followGpu))
            throw new Error('set_follow_producer_gpu failed');
        d.connect('preferred-render-device', (_o, major, minor) =>
            this._reconnectOnGpu(major, minor));
        if (!d.set_pause_effect_capabilities(Waywallen.PauseEffectCapability.BLUR))
            throw new Error('set_pause_effect_capabilities failed');
        const transitions = Waywallen.TransitionCapability.FADE |
            Waywallen.TransitionCapability.WIPE |
            Waywallen.TransitionCapability.GROW;
        if (!d.set_transition_capabilities(transitions))
            throw new Error('set_transition_capabilities failed');
        this._display = d;

        this._bindingSignalId ??= this._presentationWidget.connect('binding-staged',
            (_o, count, w, h, fourcc, backend) =>
                this._onBindingReady(count, w, h, fourcc, backend));
        // Register physical pixels (logical geometry × scale) so the producer
        // renders at native resolution, not the logical (half-res on HiDPI).
        const geom = this._monitor.get_geometry();
        const scale = this._monitor.get_scale?.() ?? this._monitor.get_scale_factor();
        this._scale = scale;
        const pw = Math.round(geom.width * scale);
        const ph = Math.round(geom.height * scale);
        this._pw = pw;
        this._ph = ph;
        this._presentationWidget.set_display(d, scale);

        d.connect('frame-ready',
            (_o, bufferGeneration, idx, seq) =>
                this._onFrameReady(bufferGeneration, idx, seq));
        d.connect('composition-config', () => this._onCompositionConfig());
        d.connect('presentation-snapshot',
            (_o, configGeneration, stateGeneration, kind, radius, active,
                transitionKind, durationMs, angle, originX, originY) =>
                this._onPresentationSnapshot(
                    configGeneration, stateGeneration, kind, radius, active,
                    transitionKind, durationMs, angle, originX, originY));
        d.connect('presentation-state',
            (_o, stateGeneration, configGeneration, active) =>
                this._onPresentationState(
                    stateGeneration, configGeneration, active));
        d.connect('disconnected',
            (_o, code, msg) => {
                if (this._display === d)
                    this._onDisconnected(code, msg);
            });

        const refreshMhz = this._monitor.get_refresh_rate() || 60000;
        d.connect('window-observation-rules', (_o, generation, applicationIds, titles, applicationIdPatterns, titlePatterns) => {
            this._writeControl({type: 'window-observation-config', config: {generation, applicationIds, titles, applicationIdPatterns, titlePatterns}});
        });
        if (!d.set_window_observation_capabilities(15))
            throw new Error('set_window_observation_capabilities failed');
        d.set_window_state(this._winFlags ?? 0);
        if (!d.begin_connect(this._opts.socketPath,
                             this._displayName,
                             this._instanceId,
                             pw, ph, refreshMhz))
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

    _reconnectOnGpu(major, minor) {
        if (this._gpuReconnectId || this._destroyed)
            return;
        this._preferredGpu = {major, minor};
        // Dispatch is still on the old display's stack; tear it down after it returns.
        this._gpuReconnectId = GLib.idle_add(GLib.PRIORITY_DEFAULT, () => {
            this._gpuReconnectId = 0;
            if (this._destroyed)
                return GLib.SOURCE_REMOVE;
            for (const key of ['_inSourceId', '_outSourceId']) {
                if (this[key]) GLib.source_remove(this[key]);
                this[key] = 0;
            }
            this._presentationWidget.set_display(null, 1);
            this._presentationWidget.clear();
            const old = this._display;
            this._display = null;
            old.disconnect();
            this._presentation = null;
            this._presentationReady = false;
            this._writeControl({type: 'reset'});
            try {
                this._initWaywallen();
            } catch (e) {
                logIndexed(this._index, `GPU reconnect failed: ${e}`);
                this._exit();
            }
            return GLib.SOURCE_REMOVE;
        });
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

    _onBindingReady(count, w, h, fourcc, backend) {
        this._texW = w;
        this._texH = h;
        this._fourcc = fourcc;
        this._backend = backend;
        logIndexed(this._index,
            `bound shadow ${w}x${h} fourcc=0x${fourcc.toString(16)} ` +
            `count=${count} backend=${backend}`);
        this._onCompositionConfig();
    }

    _onCompositionConfig() {
        // Pointer state can arrive before a producer is bound. Reapply it on
        // binding/layout changes even when the physical cursor has not moved.
        this._resendPointerMotion();
    }

    _onFrameReady(_bufferGeneration, _idx, _seq) {
        this._frames = (this._frames ?? 0) + 1;
    }

    _controlGeometry() {
        const geometry = this._monitor.get_geometry();
        return {
            x: geometry.x,
            y: geometry.y,
            width: geometry.width,
            height: geometry.height,
        };
    }

    _writeControl(frame) {
        print(encodeControlFrame({geometry: this._controlGeometry(), ...frame}).trimEnd());
    }

    _onPresentationSnapshot(configGeneration, stateGeneration, kind, radius, active,
        transitionKind, durationMs, angle, originX, originY) {
        if (configGeneration === 0) {
            this._presentation = null;
            this._presentationReady = false;
            this._updateWindowTitle();
            this._writeControl({type: 'reset'});
            return;
        }
        this._presentation = {
            config: {
                generation: configGeneration,
                pauseEffect: {kind, blur: {radius}},
                transition: {
                    kind: transitionKind,
                    durationMs,
                    angle,
                    origin: {x: originX, y: originY},
                },
            },
            state: {
                generation: stateGeneration,
                configGeneration,
                pauseEffect: {active},
            },
        };
        const type = this._presentationReady ? 'presentation-snapshot' : 'connection';
        this._writeControl({type, presentation: this._presentation});
        if (this._diagLabel)
            this._updateDiag();
    }

    _onPresentationState(stateGeneration, configGeneration, active) {
        if (!this._presentation ||
            configGeneration !== this._presentation.config.generation)
            return;
        const state = {
            generation: stateGeneration,
            configGeneration,
            pauseEffect: {active},
        };
        this._presentation.state = state;
        this._writeControl({type: 'presentation-state', state});
        if (this._diagLabel)
            this._updateDiag();
    }

    confirmPresentation(configGeneration) {
        if (!this._presentation ||
            configGeneration !== this._presentation.config.generation)
            return;
        this._presentationReady = true;
        this._updateWindowTitle();
    }

    _updateWindowTitle() {
        if (!this._window)
            return;
        const geometry = this._monitor.get_geometry();
        const hint = JSON.stringify({
            keepMinimized: true,
            keepAtBottom: true,
            keepPosition: true,
            position: [geometry.x, geometry.y],
            presentationReady: this._presentationReady,
        });
        this._window.set_title(`@${APP_ID}!${hint}|${this._index}`);
    }

    monitorGeom() {
        return this._monitor.get_geometry();
    }

    // x,y are logical monitor-local; daemon wants physical, so scale up.
    sendPointerMotion(x, y, ts) {
        const s = this._scale || 1;
        const g = this.monitorGeom();
        const inside = x >= 0 && y >= 0 && x < g.width && y < g.height;
        const px = inside ? x * s : -1;
        const py = inside ? y * s : -1;
        const previous = this._pointerMotion;
        this._pointerMotion = [px, py, ts];
        if (previous?.[0] === px && previous?.[1] === py)
            return;
        this._resendPointerMotion();
    }

    _resendPointerMotion() {
        if (this._pointerMotion) {
            const [x, y, ts] = this._pointerMotion;
            this._display?.send_pointer_motion(x, y, ts, 0);
        }
    }

    sendPointerButton(x, y, code, pressed, ts) {
        const s = this._scale || 1;
        this._display?.send_pointer_button(x * s, y * s, code, pressed, ts, 0);
    }

    sendPointerAxis(x, y, dx, dy, ts) {
        const s = this._scale || 1;
        this._display?.send_pointer_axis(x * s, y * s, dx, dy, ts, 0);
    }

    sendWindowState(flags, generation = null) {
        this._winFlags = flags;
        if (generation === null) this._display?.set_window_state(flags);
        else this._display?.set_window_observation_state(generation, flags);
        if (this._diagLabel)
            this._updateDiag();
    }

    _onDisconnected(code, msg) {
        logIndexed(this._index, `disconnected: code=${code} msg=${msg}`);
        this._presentationWidget?.clear();
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
        if (this._gpuReconnectId) {
            GLib.source_remove(this._gpuReconnectId);
            this._gpuReconnectId = 0;
        }
        if (this._inSourceId) {
            GLib.source_remove(this._inSourceId);
            this._inSourceId = 0;
        }
        if (this._outSourceId) {
            GLib.source_remove(this._outSourceId);
            this._outSourceId = 0;
        }
        if (this._diagTimerId) {
            GLib.source_remove(this._diagTimerId);
            this._diagTimerId = 0;
        }
        if (this._presentationWidget) {
            this._presentationWidget.set_display(null, 1);
            this._presentationWidget.clear();
            this._presentationWidget = null;
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

// Input forwarded by the extension over stdin (global compositor coords),
// Motion updates every monitor, including a leave sample for other outputs.
// Buttons, axes and control messages target the monitor containing the point:
//   M gx gy ts                 motion
//   B gx gy code pressed ts    button
//   A gx gy dx dy ts           axis
//   W gx gy flags              window-state
//   R gx gy config_generation  presentation snapshot accepted by Shell
function dispatchInput(line) {
    const f = line.split(' ');
    const gx = parseFloat(f[1]);
    const gy = parseFloat(f[2]);
    if (!Number.isFinite(gx) || !Number.isFinite(gy))
        return;
    if (f[0] === 'M') {
        const ts = parseInt(f[3]) || 0;
        for (const r of renderers) {
            const g = r.monitorGeom();
            r.sendPointerMotion(gx - g.x, gy - g.y, ts);
        }
        return;
    }
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
    case 'B': r.sendPointerButton(lx, ly, parseInt(f[3]) || 0,
                                  f[4] === '1', parseInt(f[5]) || 0); break;
    case 'A': r.sendPointerAxis(lx, ly, parseFloat(f[3]) || 0,
                                parseFloat(f[4]) || 0, parseInt(f[5]) || 0); break;
    case 'W': r.sendWindowState(parseInt(f[3]) || 0, f.length > 4 ? Number(f[4]) : null); break;
    case 'R': r.confirmPresentation(parseInt(f[3]) || 0); break;
    }
}

function readInputStdin() {
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
                dispatchInput(line);
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
    readInputStdin();
});

app.connect('shutdown', () => {
    for (const r of renderers)
        r.destroy();
    renderers = [];
});

const status = app.run([system.programInvocationName]);
system.exit(status);
