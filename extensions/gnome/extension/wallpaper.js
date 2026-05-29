// Holds a Clutter.Clone of the renderer's MetaWindowActor for one
// monitor. Inserted as a child of the original Background actor by
// gnomeShellOverride, so it follows the desktop's z-order, rounded
// corners (in workspace overview) and lifetime automatically.

import Clutter from 'gi://Clutter';
import GLib from 'gi://GLib';
import GObject from 'gi://GObject';
import St from 'gi://St';
import Graphene from 'gi://Graphene';

export const APPLICATION_ID = 'io.github.waywallen.WaywallenRenderer';

const FADE_IN_MS = 800;

export const LiveWallpaper = GObject.registerClass(
class LiveWallpaper extends St.Widget {
    _init(backgroundActor) {
        super._init({
            layout_manager: new Clutter.BinLayout(),
            width: backgroundActor.width,
            height: backgroundActor.height,
            x_expand: true,
            y_expand: true,
            opacity: 0,
        });
        this._backgroundActor = backgroundActor;
        this._monitorIndex = backgroundActor.monitor;

        backgroundActor.layout_manager = new Clutter.BinLayout();
        backgroundActor.add_child(this);

        this._cloneActor = null;
        this._cloneDestroyId = 0;
        this._sourceActor = null;
        this._sourceDestroyId = 0;
        this._pollId = 0;
        this._tryAttach();
    }

    _tryAttach() {
        const renderer = this._findRenderer();
        if (renderer) {
            this._cloneActor = new Clutter.Clone({
                source: renderer,
                pivot_point: new Graphene.Point({x: 0.5, y: 0.5}),
            });
            this._cloneDestroyId = this._cloneActor.connect('destroy', () => {
                this._cloneActor = null;
                this._cloneDestroyId = 0;
            });
            this._sourceActor = renderer;
            this._sourceDestroyId = renderer.connect('destroy',
                () => this._onSourceDestroyed());
            this.add_child(this._cloneActor);
            this.ease({
                opacity: 255,
                duration: FADE_IN_MS,
                mode: Clutter.AnimationMode.EASE_OUT_QUAD,
            });
            // No redraw timer: Clutter.Clone tracks the source actor's
            // queue-redraw, which fires when the renderer commits a new
            // wl_buffer. So the clone repaints exactly when there's new
            // content — a 1 fps wallpaper costs 1 repaint/s, a 60 fps one
            // costs 60. (An earlier 16 fps poll was a workaround for the
            // then-broken DMA-BUF sync that froze the source.)
            return;
        }
        this._schedulePoll();
    }

    _schedulePoll() {
        if (this._pollId !== 0)
            return;
        this._pollId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, 1000, () => {
            this._pollId = 0;
            if (!this._cloneActor)
                this._tryAttach();
            return GLib.SOURCE_REMOVE;
        });
    }

    _onSourceDestroyed() {
        this._sourceDestroyId = 0;
        this._sourceActor = null;
        if (this._cloneActor) {
            const clone = this._cloneActor;
            this._cloneActor = null;
            if (this._cloneDestroyId) {
                try { clone.disconnect(this._cloneDestroyId); } catch (_e) {}
                this._cloneDestroyId = 0;
            }
            this.opacity = 0;
            try { clone.destroy(); } catch (_e) {}
        }
        this._schedulePoll();
    }

    _findRenderer() {
        // bypass the override that hides our windows from the rest of
        // the shell — we explicitly want to see them here.
        const actors = global.get_window_actors(false);
        const ours = actors.filter(a =>
            a.meta_window.title?.includes(APPLICATION_ID));

        const numMonitors = global.display.get_n_monitors();
        if (ours.length < numMonitors)
            return null;

        const monitorIndices = ours.map(a => a.meta_window.get_monitor());
        if (new Set(monitorIndices).size !== monitorIndices.length)
            return null;

        return ours.find(a =>
            a.meta_window.get_monitor() === this._monitorIndex) ?? null;
    }

    on_destroy() {
        if (this._pollId) {
            GLib.source_remove(this._pollId);
            this._pollId = 0;
        }
        if (this._sourceActor && this._sourceDestroyId) {
            try { this._sourceActor.disconnect(this._sourceDestroyId); } catch (_e) {}
        }
        this._sourceActor = null;
        this._sourceDestroyId = 0;
        if (this._cloneActor && this._cloneDestroyId) {
            try { this._cloneActor.disconnect(this._cloneDestroyId); } catch (_e) {}
            this._cloneDestroyId = 0;
        }
        this._cloneActor = null;
        super.on_destroy?.();
    }
});
