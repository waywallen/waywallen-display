// Holds a Clutter.Clone of the renderer's MetaWindowActor for one
// monitor. Inserted as a child of the original Background actor by
// gnomeShellOverride, so it follows the desktop's z-order, rounded
// corners (in workspace overview) and lifetime automatically.

import Clutter from 'gi://Clutter';
import Cogl from 'gi://Cogl';
import GLib from 'gi://GLib';
import GObject from 'gi://GObject';
import St from 'gi://St';
import Graphene from 'gi://Graphene';
import Shell from 'gi://Shell';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';

import {BlurController} from './blur.js';
import {PauseEffectKind} from './controlCodec.js';

export const APPLICATION_ID = 'io.github.waywallen.WaywallenRenderer';
const TITLE_PREFIX = `@${APPLICATION_ID}!`;

export const WallpaperRole = Object.freeze({
    Desktop: 'desktop',
    Other: 'other',
});

export function rendererPresentationReady(actor) {
    const title = actor?.meta_window?.title;
    if (!title?.startsWith(TITLE_PREFIX))
        return false;
    const payload = title.slice(TITLE_PREFIX.length).split('|', 1)[0];
    try {
        return JSON.parse(payload).presentationReady === true;
    } catch (_e) {
        return false;
    }
}

const FADE_IN_MS = 800;

// From LiveWallpaper: Meta.BackgroundContent clips its own pixels, not child clones.
const CLIP_DECLARATIONS = `
uniform vec4 bounds;
uniform float clip_radius;
uniform vec2 pixel_step;

float rounded_rect_coverage(vec2 p) {
    float center_left = bounds.x + clip_radius;
    float center_right = bounds.z - clip_radius;
    float center_x;
    if (p.x < center_left)
        center_x = center_left;
    else if (p.x > center_right)
        center_x = center_right;
    else
        return 1.0;

    float center_top = bounds.y + clip_radius;
    float center_bottom = bounds.w - clip_radius;
    float center_y;
    if (p.y < center_top)
        center_y = center_top;
    else if (p.y > center_bottom)
        center_y = center_bottom;
    else
        return 1.0;

    vec2 delta = p - vec2(center_x, center_y);
    float dist_squared = dot(delta, delta);
    float outer_radius = clip_radius + 0.5;
    if (dist_squared >= outer_radius * outer_radius)
        return 0.0;
    float inner_radius = clip_radius - 0.5;
    if (dist_squared <= inner_radius * inner_radius)
        return 1.0;
    return outer_radius - sqrt(dist_squared);
}
`;

const CLIP_CODE = `
vec2 texture_coord = cogl_tex_coord0_in.xy / pixel_step;
bool inside = texture_coord.x >= bounds.x && texture_coord.x <= bounds.z
           && texture_coord.y >= bounds.y && texture_coord.y <= bounds.w;
if (clip_radius > 0.0 && !inside)
    cogl_color_out = vec4(0.0);
else if (clip_radius > 0.0)
    cogl_color_out *= rounded_rect_coverage(texture_coord);
`;

function enlargeForEffects(x1, y1, width, height) {
    if (width <= 0 || height <= 0)
        return [x1, y1];
    const x2 = Math.ceil(x1 + width + 0.75);
    const y2 = Math.ceil(y1 + height + 0.75);
    return [x2 - Math.round(width) - 3, y2 - Math.round(height) - 3];
}

function fboOffset(actor) {
    const volume = actor.get_paint_volume();
    if (volume) {
        const origin = volume.get_origin();
        return enlargeForEffects(
            origin.x, origin.y, volume.get_width(), volume.get_height()
        ).map(Math.trunc);
    }
    const box = actor.get_allocation_box();
    const [x1, y1] = enlargeForEffects(
        box.x1, box.y1, box.x2 - box.x1, box.y2 - box.y1
    );
    return [Math.trunc(x1 - box.x1), Math.trunc(y1 - box.y1)];
}

const RoundedClipEffect = GObject.registerClass(
class WaywallenRoundedClipEffect extends Shell.GLSLEffect {
    constructor() {
        super();
        this._bounds = [0, 0, 0, 0];
        this._radius = 0;
        this._dirty = true;
        this._textureWidth = 0;
        this._textureHeight = 0;
        this._textureScale = 0;
        this._offsetX = 0;
        this._offsetY = 0;
    }

    vfunc_build_pipeline() {
        this.add_glsl_snippet(
            Cogl.SnippetHook.FRAGMENT, CLIP_DECLARATIONS, CLIP_CODE, false
        );
    }

    vfunc_paint_target(node, paintContext) {
        this._uploadUniforms();
        super.vfunc_paint_target(node, paintContext);
    }

    setGeometry(bounds, radius) {
        this._bounds = bounds;
        this._radius = radius;
        this._dirty = true;
    }

    _uploadUniforms() {
        const texture = this.get_texture();
        const actor = this.get_actor();
        if (!texture || !actor)
            return;
        const width = texture.get_width();
        const height = texture.get_height();
        const scale = actor.get_resource_scale();
        const [offsetX, offsetY] = fboOffset(actor);
        if (!this._dirty &&
            width === this._textureWidth &&
            height === this._textureHeight &&
            scale === this._textureScale &&
            offsetX === this._offsetX &&
            offsetY === this._offsetY)
            return;

        this._dirty = false;
        this._textureWidth = width;
        this._textureHeight = height;
        this._textureScale = scale;
        this._offsetX = offsetX;
        this._offsetY = offsetY;

        this.set_uniform_float(
            this.get_uniform_location('pixel_step'), 2, [1 / width, 1 / height]
        );
        const [x1, y1, x2, y2] = this._bounds;
        this.set_uniform_float(this.get_uniform_location('bounds'), 4, [
            (x1 - offsetX) * scale,
            (y1 - offsetY) * scale,
            (x2 - offsetX) * scale,
            (y2 - offsetY) * scale,
        ]);
        this.set_uniform_float(
            this.get_uniform_location('clip_radius'), 1, [this._radius * scale]
        );
    }
});

export const LiveWallpaper = GObject.registerClass(
class LiveWallpaper extends St.Widget {
    _init(backgroundActor, role = WallpaperRole.Other, rendererAvailable = false,
        rendererLauncher = null, onDestroyed = null) {
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
        this._clipEffect = null;
        this._clipContent = backgroundActor.content;
        this._clipRadiusId = this._clipContent?.connect('notify::rounded-clip-radius',
            () => this.queue_relayout()) ?? 0;
        this._wwDestroyed = false;
        this._onDestroyed = onDestroyed;
        this._destroyId = this.connect('destroy', () => this._cleanup());
        this._backgroundDestroyId = backgroundActor.connect('destroy', () => {
            // The parent is disposing; do not read its content during child teardown.
            this._backgroundActor = null;
            this._backgroundDestroyId = 0;
            this._cleanup();
        });

        backgroundActor.layout_manager = new Clutter.BinLayout();
        backgroundActor.add_child(this);

        this._cloneActor = null;
        this._cloneDestroyId = 0;
        this._sourceActor = null;
        this._sourceDestroyId = 0;
        this._pollId = 0;
        if (this._role === WallpaperRole.Desktop)
            this.set_style('background-color: black;');
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
        this._syncClip(box);
    }

    _syncClip(box) {
        const monitor = Main.layoutManager.monitors[this._monitorIndex];
        if (!monitor || monitor.width <= 0 || monitor.height <= 0)
            return;
        const sx = box.get_width() / monitor.width;
        const sy = box.get_height() / monitor.height;
        const radius = (this._clipContent?.rounded_clip_radius ?? 0) * sx;
        if (radius < 0.5 || sy <= 0) {
            if (this._clipEffect) {
                this.remove_effect(this._clipEffect);
                this._clipEffect = null;
            }
            return;
        }

        const work = Main.layoutManager.getWorkAreaForMonitor(this._monitorIndex);
        const x = (work.x - monitor.x) * sx;
        const y = (work.y - monitor.y) * sy;
        if (!this._clipEffect) {
            this._clipEffect = new RoundedClipEffect();
            this.add_effect(this._clipEffect);
        }
        this._clipEffect.setGeometry(
            [x, y, x + work.width * sx, y + work.height * sy], radius);
    }

    _tryAttach() {
        if (this._wwDestroyed || this._cloneActor)
            return;
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
                this._detachRenderer();
                this._showFallback();
                this._schedulePoll();
            });
            this._sourceActor = renderer;
            this._sourceDestroyId = renderer.connect('destroy',
                () => this._onSourceDestroyed());
            this.add_child(this._cloneActor);
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
            // No redraw timer: Clutter.Clone repaints on the source's
            // queue-redraw, i.e. exactly when the renderer commits a buffer.
            return;
        }
        this._schedulePoll();
    }

    _schedulePoll() {
        if (this._wwDestroyed || this._pollId !== 0 || !this._rendererAvailable)
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
                if (this._origBrightness === undefined) {
                    this._origBrightness = content.brightness;
                    this._origVignette = content.vignette;
                }
                // Mutter only applies brightness when vignette is enabled
                // (see meta-background-content.c); without this line the
                // dim is a no-op on overview workspace previews and the
                // gsettings wallpaper bleeds through the clone's alpha.
                content.vignette = true;
                content.brightness = 0;
            } else if (this._origBrightness !== undefined) {
                content.vignette = this._origVignette;
                content.brightness = this._origBrightness;
                this._origBrightness = undefined;
                this._origVignette = undefined;
            }
        } catch (_e) {}
    }

    _onSourceDestroyed() {
        this._sourceDestroyId = 0;
        this._sourceActor = null;
        this._detachRenderer();
        this._showFallback();
        this._schedulePoll();
    }

    _detachRenderer() {
        if (this._sourceActor && this._sourceDestroyId)
            this._sourceActor.disconnect(this._sourceDestroyId);
        this._sourceActor = null;
        this._sourceDestroyId = 0;
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
    }

    _showFallback() {
        if (this._wwDestroyed)
            return;
        this._cloneActor?.remove_all_transitions();
        if (this._cloneActor)
            this._cloneActor.opacity = 0;
        if (this._role === WallpaperRole.Desktop) {
            this.opacity = 255;
            this._dimBackdrop(true);
        } else {
            this.opacity = 0;
            this._dimBackdrop(false);
        }
    }

    setRendererAvailable(available) {
        if (this._wwDestroyed || this._rendererAvailable === available)
            return;
        this._rendererAvailable = available;
        if (!available) {
            this._cancelPoll();
            this._detachRenderer();
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
            this._dimBackdrop(true);
            return;
        }
        this._tryAttach();
    }

    setRendererLauncher(launcher) {
        if (this._wwDestroyed || this._rendererLauncher === launcher)
            return;
        this._cancelPoll();
        this._detachRenderer();
        this._rendererLauncher = launcher;
        this._showFallback();
        this._tryAttach();
    }

    _findRenderer() {
        const actors = global.display.list_all_windows()
            .map(window => window.get_compositor_private())
            .filter(actor => actor);
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
        if (this._wwDestroyed || this._role !== WallpaperRole.Desktop)
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

    _cancelPoll() {
        if (this._pollId) {
            GLib.source_remove(this._pollId);
            this._pollId = 0;
        }
    }

    _cleanup() {
        if (this._wwDestroyed)
            return;
        this._wwDestroyed = true;
        const onDestroyed = this._onDestroyed;
        this._onDestroyed = null;
        onDestroyed?.(this);
        this._cancelPoll();
        if (this._clipRadiusId)
            this._clipContent.disconnect(this._clipRadiusId);
        this._clipRadiusId = 0;
        this._clipContent = null;
        if (this._clipEffect) {
            this.remove_effect(this._clipEffect);
            this._clipEffect = null;
        }
        this._detachRenderer();
        this._dimBackdrop(false);
        if (this._backgroundActor && this._backgroundDestroyId)
            this._backgroundActor.disconnect(this._backgroundDestroyId);
        this._backgroundDestroyId = 0;
        this._backgroundActor = null;
        this._rendererLauncher = null;
        this._presentation = null;
        if (this._destroyId) {
            this.disconnect(this._destroyId);
            this._destroyId = 0;
        }
    }
});
