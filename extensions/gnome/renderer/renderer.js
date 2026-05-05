#!/usr/bin/env -S gjs -m
//
// waywallen-display-gnome-renderer
//
// One Gtk.ApplicationWindow per Gdk.Monitor; each window holds a
// Gtk.Picture whose paintable is swapped per-frame to a GdkGLTexture
// wrapping the GL texture libwaywallen_display imported from the daemon.
// GTK 4.12+'s GdkGLTextureBuilder lets us hand a GL texture id (created
// in our own GdkGLContext) to GTK without writing any GL drawing code
// in JS — GTK does the cross-context wait + final blit internally.
//
// Lifetime / fd ownership is documented inline at every transfer.

import GLib from 'gi://GLib';
import Gio from 'gi://Gio';
import Gdk from 'gi://Gdk?version=4.0';
import Gtk from 'gi://Gtk?version=4.0';
import GdkWayland from 'gi://GdkWayland?version=4.0';
import Waywallen from 'gi://Waywallen?version=1.0';
import system from 'system';

const APP_ID = 'io.github.waywallen.WaywallenRenderer';

// Mirrors the values exposed in WwHandshakeResult.
const HS_DONE = 1;
const HS_NEED_READ = 2;
const HS_NEED_WRITE = 3;
const HS_PROGRESS = 4;

const HS_READY = 6; // WwHandshakeState.READY

function logIndexed(idx, msg) {
    printerr(`[ww-renderer ${idx}] ${msg}`);
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

        this._window = null;
        this._picture = null;
        this._glContext = null;
        this._display = null;

        this._inSourceId = 0;
        this._outSourceId = 0;

        this._textureCount = 0;
        this._texWidth = 0;
        this._texHeight = 0;
        this._glTextures = [];

        // Lazy-release: hold previous frame's release_fd until next
        // frame_ready arrives, then signal it. Buys one extra frame of
        // worst-case latency for the daemon's reaper but lets GTK finish
        // sampling the previous texture before the buffer is reused.
        this._prevReleaseFd = -1;

        this._destroyed = false;
    }

    build(app) {
        this._window = new Gtk.ApplicationWindow({
            application: app,
            decorated: false,
            resizable: false,
        });

        // Title is consumed by the gnome-shell extension to identify
        // this window's monitor. JSON payload is reserved for future
        // hint flags.
        const titleHint = JSON.stringify({});
        this._window.set_title(`@${APP_ID}!${titleHint}|${this._index}`);

        this._picture = new Gtk.Picture({
            can_shrink: true,
            content_fit: Gtk.ContentFit.COVER,
            hexpand: true,
            vexpand: true,
        });
        this._window.set_child(this._picture);

        const geom = this._monitor.get_geometry();
        this._window.set_default_size(geom.width, geom.height);

        // realize fires synchronously inside present() on Wayland. Hook
        // it before present() so we can fullscreen-on-monitor on the
        // first map.
        this._window.connect('realize', () => this._onRealize());
        this._window.fullscreen_on_monitor(this._monitor);
        this._window.present();
    }

    _onRealize() {
        try {
            this._initWaywallen();
        } catch (e) {
            logIndexed(this._index, `init failed: ${e}`);
            this._exit(1);
        }
    }

    _initWaywallen() {
        const surface = this._window.get_surface();
        if (!surface)
            throw new Error('window has no surface yet');

        // Our own GL context, used only to call create_gl_texture in.
        // Sharing the EGLDisplay with GTK's compositor context lets GTK
        // sample these textures cross-context.
        const ctx = surface.create_gl_context();
        ctx.realize();
        ctx.make_current();
        this._glContext = ctx;

        // EGLDisplay handle. GdkWaylandDisplay is a subclass of GdkDisplay
        // when running on Wayland; the prototype-call pattern works
        // because the actual instance IS a WaylandDisplay.
        const gdkDisplay = Gdk.Display.get_default();
        const eglDisplay = GdkWayland.WaylandDisplay.prototype
            .get_egl_display.call(gdkDisplay);
        if (!eglDisplay)
            throw new Error('GdkWaylandDisplay has no EGLDisplay');

        const d = Waywallen.Display.new();
        if (!d.bind_egl(eglDisplay, null))
            throw new Error('Waywallen.Display.bind_egl failed');
        this._display = d;

        d.connect('textures-ready',
            (_o, count, w, h, fourcc, modifier, backend) =>
                this._onTexturesReady(count, w, h, fourcc, modifier, backend));
        d.connect('textures-releasing', () => this._onTexturesReleasing());
        d.connect('frame-ready',
            (_o, idx, seq, fd) => this._onFrameReady(idx, seq, fd));
        d.connect('disconnected',
            (_o, code, msg) => this._onDisconnected(code, msg));

        const geom = this._monitor.get_geometry();
        const refreshMhz = this._monitor.get_refresh_rate() || 60000;
        if (!d.begin_connect(this._opts.socketPath,
                             this._opts.displayName,
                             this._opts.instanceId,
                             geom.width, geom.height, refreshMhz))
            throw new Error('begin_connect failed');

        const fd = d.get_fd();
        if (fd < 0)
            throw new Error('display fd is -1 after begin_connect');

        // Always watch for readability; advance_handshake's NEED_WRITE
        // toggles the OUT watch.
        this._inSourceId = GLib.unix_fd_add(GLib.PRIORITY_DEFAULT, fd,
                                            GLib.IOCondition.IN,
                                            () => this._driveIO());
        this._driveIO();
    }

    _setOutWatch(want) {
        const fd = this._display ? this._display.get_fd() : -1;
        if (want && this._outSourceId === 0 && fd >= 0) {
            this._outSourceId = GLib.unix_fd_add(GLib.PRIORITY_DEFAULT, fd,
                                                  GLib.IOCondition.OUT,
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
            // PROGRESS, NEED_READ, NEED_WRITE all keep waiting on poll.
            // DONE means transitioned to READY this iteration; loop into
            // dispatch immediately so we don't drop the first batch of
            // frames already buffered on the socket.
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

    _onTexturesReady(count, w, h, fourcc, modifier, backend) {
        logIndexed(this._index,
            `textures-ready: count=${count} ${w}x${h} ` +
            `fourcc=0x${fourcc.toString(16)} backend=${backend}`);

        this._releaseGlTextures();
        this._texWidth = w;
        this._texHeight = h;
        this._textureCount = count;

        this._glContext.make_current();
        for (let i = 0; i < count; i++) {
            const [ok, tex] = this._display.create_gl_texture(i);
            if (!ok) {
                logIndexed(this._index, `create_gl_texture(${i}) failed`);
                this._glTextures.push(0);
            } else {
                this._glTextures.push(tex);
            }
        }
    }

    _onTexturesReleasing() {
        logIndexed(this._index, 'textures-releasing');
        // Drop any displayed paintable first so GTK lets go of the
        // texture before we delete the underlying GL name.
        if (this._picture)
            this._picture.set_paintable(null);
        this._releaseGlTextures();
    }

    _releaseGlTextures() {
        if (!this._display)
            return;
        for (let i = 0; i < this._glTextures.length; i++) {
            if (this._glTextures[i])
                this._display.delete_gl_texture(i);
        }
        this._glTextures = [];
        this._textureCount = 0;
    }

    _onFrameReady(idx, seq, fd) {
        // Lazy release: signal previous frame's fd now that we're
        // about to swap to a new one. The previous frame has already
        // been handed to GTK.set_paintable; by the time the next
        // frame arrives, GTK has long finished sampling it.
        if (this._prevReleaseFd >= 0) {
            Waywallen.Display.signal_release_syncobj(this._prevReleaseFd);
            this._prevReleaseFd = -1;
        }
        this._prevReleaseFd = Waywallen.Display.dup_release_fd(fd);

        if (idx >= this._glTextures.length || !this._glTextures[idx])
            return;

        const builder = new Gdk.GLTextureBuilder();
        builder.set_context(this._glContext);
        builder.set_id(this._glTextures[idx]);
        builder.set_width(this._texWidth);
        builder.set_height(this._texHeight);
        // Daemon's textures are sRGB-typed BGRA8; if the host's fourcc
        // ever deviates we'll need a switch on the textures-ready code.
        builder.set_format(Gdk.MemoryFormat.B8G8R8A8);
        const tex = builder.build(null);
        this._picture.set_paintable(tex);
    }

    _onDisconnected(code, msg) {
        logIndexed(this._index, `disconnected: code=${code} msg=${msg}`);
        // Quit the whole app: extension respawns us.
        this._exit(0);
    }

    _exit(code) {
        if (this._destroyed)
            return;
        this._destroyed = true;
        const app = Gtk.Application.get_default();
        if (app)
            app.quit();
        // Code propagation isn't guaranteed via Gtk.Application.quit but
        // ARGV exit hooks below handle it on the activate path.
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
        if (this._prevReleaseFd >= 0) {
            Waywallen.Display.close_fd(this._prevReleaseFd);
            this._prevReleaseFd = -1;
        }
        if (this._picture)
            this._picture.set_paintable(null);
        if (this._display) {
            this._releaseGlTextures();
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
});

app.connect('shutdown', () => {
    for (const r of renderers)
        r.destroy();
    renderers = [];
});

const status = app.run([system.programInvocationName]);
system.exit(status);
