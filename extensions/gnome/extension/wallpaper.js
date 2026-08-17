// Holds a Clutter.Clone of the renderer's MetaWindowActor for one
// monitor. Inserted as a child of the original Background actor by
// gnomeShellOverride, so it follows the desktop's z-order, rounded
// corners (in workspace overview) and lifetime automatically.

import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import GObject from 'gi://GObject';
import St from 'gi://St';
import Graphene from 'gi://Graphene';

import {BlurController} from './blur.js';
import {PauseEffectKind} from './controlCodec.js';

export const APPLICATION_ID = 'io.github.waywallen.WaywallenRenderer';
const TITLE_PREFIX = `@${APPLICATION_ID}!`;

export const WallpaperRole = Object.freeze({
    Desktop: 'desktop',
    Other: 'other',
});

export function rendererTitleHint(actor) {
    const title = actor?.meta_window?.title;
    if (!title?.startsWith(TITLE_PREFIX))
        return null;
    const payload = title.slice(TITLE_PREFIX.length).split('|', 1)[0];
    try {
        return JSON.parse(payload);
    } catch (_e) {
        return null;
    }
}

export function rendererPresentationReady(actor) {
    return rendererTitleHint(actor)?.presentationReady === true;
}

export function rendererHasFrame(actor) {
    return rendererTitleHint(actor)?.hasFrame === true;
}

const FADE_IN_MS = 800;

export const LiveWallpaper = GObject.registerClass(
class LiveWallpaper extends St.Widget {
    _init(backgroundActor, role = WallpaperRole.Other, rendererAvailable = false,
        rendererLauncher = null, extensionPath = '') {
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
            opacity: role === WallpaperRole.Desktop ? 255 : 0,
        });
        this._backgroundActor = backgroundActor;
        this._monitorIndex = backgroundActor.monitor;
        this._role = role;
        this._rendererAvailable = rendererAvailable;
        this._rendererLauncher = rendererLauncher;
        this._presentation = null;
        this._blurController = null;

        backgroundActor.layout_manager = new Clutter.BinLayout();
        backgroundActor.add_child(this);

        this._cloneActor = null;
        this._cloneDestroyId = 0;
        this._sourceActor = null;
        this._sourceDestroyId = 0;
        this._pollId = 0;
        this._placeholder = null;
        this._placeholderIcon = null;
        this._extensionPath = extensionPath;
        this._titleNotifyId = 0;
        if (this._role === WallpaperRole.Desktop)
            this.set_style('background-color: #D85A30;');
        this._ensurePlaceholder();
        this._tryAttach();
        this._syncPlaceholder();
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
        const placeholder = this._placeholder;
        if (placeholder && placeholder.visible) {
            const iconSize = Math.max(64,
                Math.round(Math.min(box.get_width(), box.get_height()) * 0.22));
            if (this._placeholderIcon && this._placeholderIcon.icon_size !== iconSize)
                this._placeholderIcon.icon_size = iconSize;
            const [, natWidth] = placeholder.get_preferred_width(-1);
            const [, natHeight] = placeholder.get_preferred_height(-1);
            const x = Math.round((box.get_width() - natWidth) / 2);
            const y = Math.round((box.get_height() - natHeight) / 2);
            placeholder.allocate(new Clutter.ActorBox({
                x1: x,
                y1: y,
                x2: x + natWidth,
                y2: y + natHeight,
            }));
        }
    }

    _tryAttach() {
        if (!this._rendererAvailable) {
            this._showFallback();
            return;
        }
        const renderer = this._findRenderer();
        if (renderer) {
            this._cloneActor = new Clutter.Clone({
                source: renderer,
                opacity: 0,
            });
            this._cloneDestroyId = this._cloneActor.connect('destroy', () => {
                this._cloneActor = null;
                this._cloneDestroyId = 0;
            });
            this._sourceActor = renderer;
            this._sourceDestroyId = renderer.connect('destroy',
                () => this._onSourceDestroyed());
            this.add_child(this._cloneActor);
            this._connectTitleNotify(renderer);
            if (this._role === WallpaperRole.Desktop) {
                this._blurController = new BlurController(
                    this._cloneActor, 'waywallen-desktop-blur');
                this._applyPresentation();
            }
            this.opacity = 255;
            this._cloneActor.ease({
                opacity: 255,
                duration: FADE_IN_MS,
                mode: Clutter.AnimationMode.EASE_OUT_QUAD,
            });
            // Black out the gsettings placeholder behind us (see _dimBackdrop).
            this._dimBackdrop(true);
            this._syncPlaceholder();
            return;
        }
        this._syncPlaceholder();
        this._schedulePoll();
    }

    _schedulePoll() {
        if (this._pollId !== 0 || !this._rendererAvailable)
            return;
        this._pollId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, 1000, () => {
            this._pollId = 0;
            if (!this._cloneActor)
                this._tryAttach();
            return GLib.SOURCE_REMOVE;
        });
    }

    // The gsettings MetaBackground behind us is a placeholder solid color.
    // Where the clone has alpha or at clip edges it bleeds through as a
    // colored veil — most visible in the overview workspace preview, which
    // renders that content at brightness 0.5. While we are actively covering
    // the actor, pin the backdrop to black so any bleed reads as shadow.
    // Non-desktop backgrounds restore their normal brightness when the source
    // goes away; the desktop keeps the disconnected fallback black.
    _dimBackdrop(dim) {
        try {
            const content = this._backgroundActor?.content;
            if (!content)
                return;
            if (dim) {
                if (this._origBrightness === undefined)
                    this._origBrightness = content.brightness;
                content.brightness = 0;
            } else if (this._origBrightness !== undefined) {
                content.brightness = this._origBrightness;
                this._origBrightness = undefined;
            }
        } catch (_e) {}
    }

    _onSourceDestroyed() {
        this._disconnectTitleNotify();
        this._sourceDestroyId = 0;
        this._sourceActor = null;
        this._blurController?.destroy();
        this._blurController = null;
        if (this._cloneActor) {
            const clone = this._cloneActor;
            this._cloneActor = null;
            if (this._cloneDestroyId) {
                try { clone.disconnect(this._cloneDestroyId); } catch (_e) {}
                this._cloneDestroyId = 0;
            }
            try { clone.destroy(); } catch (_e) {}
        }
        this._showFallback();
        this._schedulePoll();
    }

    _showFallback() {
        this._cloneActor?.remove_all_transitions();
        if (this._cloneActor)
            this._cloneActor.opacity = 0;
        if (this._role === WallpaperRole.Desktop) {
            this.opacity = 255;
            this._dimBackdrop(true);
            this._showPlaceholder(true);
        } else {
            this.opacity = 0;
            this._dimBackdrop(false);
            this._showPlaceholder(false);
        }
    }

    _ensurePlaceholder() {
        if (this._placeholder || this._role !== WallpaperRole.Desktop)
            return;
        const iconPath = GLib.build_filenamev([this._extensionPath, 'waywallen.svg']);
        const box = new St.BoxLayout({
            vertical: true,
            x_expand: false,
            y_expand: false,
            style: 'spacing: 12px;',
        });
        const file = Gio.File.new_for_path(iconPath);
        if (file.query_exists(null)) {
            this._placeholderIcon = new St.Icon({
                gicon: new Gio.FileIcon({file}),
                icon_size: 128,
                x_align: Clutter.ActorAlign.CENTER,
            });
            box.add_child(this._placeholderIcon);
        }
        box.add_child(new St.Label({
            text: 'No wallpaper selected',
            x_align: Clutter.ActorAlign.CENTER,
            style: 'color: white; font-size: 22px; font-weight: 600;',
        }));
        box.add_child(new St.Label({
            text: 'Open Waywallen and select a wallpaper.',
            x_align: Clutter.ActorAlign.CENTER,
            style: 'color: #cdd6f4; font-size: 16px;',
        }));
        box.visible = false;
        this.add_child(box);
        this._placeholder = box;
    }

    _connectTitleNotify(renderer) {
        this._disconnectTitleNotify();
        const win = renderer?.meta_window;
        if (!win)
            return;
        this._titleNotifyId = win.connect('notify::title', () => this._syncPlaceholder());
    }

    _disconnectTitleNotify() {
        const win = this._sourceActor?.meta_window;
        if (win && this._titleNotifyId) {
            try { win.disconnect(this._titleNotifyId); } catch (_e) {}
        }
        this._titleNotifyId = 0;
    }

    _syncPlaceholder() {
        if (this._role !== WallpaperRole.Desktop) {
            this._showPlaceholder(false);
            return;
        }
        const show = !this._cloneActor || !rendererHasFrame(this._sourceActor);
        this._showPlaceholder(show);
        if (show && this._placeholder && this._cloneActor)
            this._placeholder.raise_top();
    }

    _showPlaceholder(show) {
        this._ensurePlaceholder();
        if (!this._placeholder)
            return;
        this._placeholder.visible = show;
        if (show)
            this.queue_relayout();
    }

    setRendererAvailable(available) {
        if (this._rendererAvailable === available)
            return;
        this._rendererAvailable = available;
        if (!available) {
            this._showFallback();
            return;
        }
        if (this._cloneActor) {
            this.opacity = 255;
            this._cloneActor.ease({
                opacity: 255,
                duration: FADE_IN_MS,
                mode: Clutter.AnimationMode.EASE_OUT_QUAD,
            });
            this._syncPlaceholder();
            return;
        }
        this._tryAttach();
    }

    setRendererLauncher(launcher) {
        this._rendererLauncher = launcher;
    }

    _findRenderer() {
        // false bypasses the override that hides our windows elsewhere.
        const actors = global.get_window_actors(false);
        const ours = actors.filter(a =>
            a.meta_window.title?.includes(APPLICATION_ID) &&
            this._rendererLauncher?.ownsWindow(a.meta_window) &&
            rendererPresentationReady(a));

        const numMonitors = global.display.get_n_monitors();
        if (ours.length < numMonitors)
            return null;

        const monitorIndices = ours.map(a => a.meta_window.get_monitor());
        if (new Set(monitorIndices).size !== monitorIndices.length)
            return null;

        return ours.find(a =>
            a.meta_window.get_monitor() === this._monitorIndex) ?? null;
    }

    setPresentation(presentation) {
        if (this._role !== WallpaperRole.Desktop)
            return;
        this._presentation = presentation;
        this._applyPresentation();
    }

    get role() {
        return this._role;
    }

    get monitorIndex() {
        return this._monitorIndex;
    }

    _applyPresentation() {
        const presentation = this._presentation;
        if (!presentation) {
            this._blurController?.setState(false, false, 30);
            return;
        }
        this._blurController?.setState(
            presentation.config.pauseEffect.kind === PauseEffectKind.BLUR,
            presentation.state.pauseEffect.active,
            presentation.config.pauseEffect.blur.radius);
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
        this._disconnectTitleNotify();
        this._sourceActor = null;
        this._sourceDestroyId = 0;
        this._blurController?.destroy();
        this._blurController = null;
        if (this._cloneActor && this._cloneDestroyId) {
            try { this._cloneActor.disconnect(this._cloneDestroyId); } catch (_e) {}
            this._cloneDestroyId = 0;
        }
        this._cloneActor = null;
        this._placeholder = null;
        this._placeholderIcon = null;
        this._dimBackdrop(false);
        super.on_destroy?.();
    }
});
