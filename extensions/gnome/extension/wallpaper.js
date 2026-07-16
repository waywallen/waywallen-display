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
            // FixedLayout: we position the clone ourselves in vfunc_allocate
            // (top-left origin, scaled to fill). BinLayout centered the
            // monitor-sized clone inside the smaller overview preview box,
            // which misaligned it and made it collapse when the preview
            // shrank for the app grid.
            layout_manager: new Clutter.FixedLayout(),
            // No explicit width/height: MetaBackgroundActor is content-driven,
            // and its width/height props are 0 in the overview. We expand to
            // fill whatever the parent allocates and report no preferred size
            // (see vfuncs) so we don't distort the overview workspace layout.
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

    // Report no preferred size: the clone's natural (monitor) size would
    // otherwise feed back into the overview workspace-preview layout and
    // distort it.
    vfunc_get_preferred_width(_forHeight) {
        return [0, 0];
    }
    vfunc_get_preferred_height(_forWidth) {
        return [0, 0];
    }

    // Scale the clone from the top-left to fill our ACTUAL allocation. The
    // backing actor's width/height props stay 0 in the content-driven overview
    // path, so the allocation box is the source of truth. Pinning to (0,0)
    // with pivot (0,0) fills the preview exactly in both the desktop and the
    // overview (including when the preview shrinks for the app grid).
    vfunc_allocate(box) {
        super.vfunc_allocate(box);
        const clone = this._cloneActor;
        if (clone && clone.source && clone.source.width > 0) {
            const sx = box.get_width() / clone.source.width;
            const sy = box.get_height() / clone.source.height;
            clone.set_position(0, 0);
            if (Math.abs((clone.scale_x ?? 1) - sx) > 0.001 ||
                Math.abs((clone.scale_y ?? 1) - sy) > 0.001)
                clone.set_scale(sx, sy);
        }
    }

    _tryAttach() {
        const renderer = this._findRenderer();
        if (renderer) {
            this._cloneActor = new Clutter.Clone({
                source: renderer,
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
            // No redraw timer: Clutter.Clone repaints on the source's
            // queue-redraw, i.e. exactly when the renderer commits a buffer.
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
        // false bypasses the override that hides our windows elsewhere.
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
        // Mark first so GnomeShellOverride.disable() can skip us when GNOME
        // has already destroyed our parent backgroundActor — avoids the
        // "already disposed" warning (and any GC-sweep jitter) from a
        // redundant second destroy() at extension teardown.
        this._wwDestroyed = true;
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
