import Clutter from 'gi://Clutter';
import GLib from 'gi://GLib';
import Meta from 'gi://Meta';
import Shell from 'gi://Shell';

import {InjectionManager} from 'resource:///org/gnome/shell/extensions/extension.js';
import * as Background from 'resource:///org/gnome/shell/ui/background.js';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as Workspace from 'resource:///org/gnome/shell/ui/workspace.js';
import * as WorkspaceThumbnail from 'resource:///org/gnome/shell/ui/workspaceThumbnail.js';

import * as Wallpaper from './wallpaper.js';
import {BlurController} from './blur.js';
import {PauseEffectKind} from './controlCodec.js';

const APPLICATION_ID = Wallpaper.APPLICATION_ID;

export class GnomeShellOverride {
    constructor(settings) {
        this._injection = new InjectionManager();
        this._settings = settings;
        // Held by LiveWallpaper -> nothing; we only iterate on disable
        // and let Clutter destruction cascades clean up otherwise.
        // Using a Set (not Map) means we don't need to wire a destroy
        // signal here — wiring one risks Gjs-CRITICAL during GC sweep
        // (the handler can fire after the JS context is being torn
        // down). Stale entries are pruned at disable() / on next switch.
        this._wallpaperActors = new Set();
        this._desktopPresentation = new Map();
        this._rendererAvailable = false;
        this._rendererLauncher = null;
    }

    enable() {
        const self = this;

        this._injection.overrideMethod(
            Background.BackgroundManager.prototype,
            '_createBackgroundActor',
            originalMethod => function () {
                const backgroundActor = originalMethod.call(this);
                const role = this._container === Main.layoutManager._backgroundGroup
                    ? Wallpaper.WallpaperRole.Desktop
                    : Wallpaper.WallpaperRole.Other;
                this.waywallenActor = new Wallpaper.LiveWallpaper(
                    backgroundActor, role, self._rendererAvailable, self._rendererLauncher);
                self._wallpaperActors.add(this.waywallenActor);
                if (role === Wallpaper.WallpaperRole.Desktop)
                    self._applyPresentationToActor(this.waywallenActor);
                return backgroundActor;
            });

        // Hide the renderer windows from various enumerations. Keep them
        // matched by window title so we don't accidentally hide a real
        // user-launched program.
        const isRenderer = w => w?.title?.includes(APPLICATION_ID);

        this._injection.overrideMethod(
            Shell.Global.prototype, 'get_window_actors',
            originalMethod => function (hideRenderer = true) {
                const list = originalMethod.call(this);
                return hideRenderer
                    ? list.filter(a => !isRenderer(a.meta_window))
                    : list;
            });

        this._injection.overrideMethod(
            Workspace.Workspace.prototype, '_isOverviewWindow',
            originalMethod => function (window) {
                return isRenderer(window)
                    ? false
                    : originalMethod.apply(this, [window]);
            });

        this._injection.overrideMethod(
            WorkspaceThumbnail.WorkspaceThumbnail.prototype, '_isOverviewWindow',
            originalMethod => function (window) {
                return isRenderer(window)
                    ? false
                    : originalMethod.apply(this, [window]);
            });

        this._injection.overrideMethod(
            Meta.Display.prototype, 'get_tab_list',
            originalMethod => function (type, workspace) {
                const ws = originalMethod.apply(this, [type, workspace]);
                return ws.filter(w => !isRenderer(w));
            });

        this._injection.overrideMethod(
            Shell.WindowTracker.prototype, 'get_window_app',
            originalMethod => function (window) {
                return isRenderer(window)
                    ? null
                    : originalMethod.apply(this, [window]);
            });

        this._injection.overrideMethod(
            Shell.App.prototype, 'get_windows',
            originalMethod => function () {
                return originalMethod.call(this).filter(w => !isRenderer(w));
            });

        this._injection.overrideMethod(
            Shell.App.prototype, 'get_n_windows',
            _ => function () {
                return this.get_windows().length;
            });

        this._injection.overrideMethod(
            Shell.AppSystem.prototype, 'get_running',
            originalMethod => function () {
                return originalMethod.call(this).filter(a => a.get_n_windows() > 0);
            });

        this._readOverviewPrefs();
        this._overviewSettingsIds = [];
        if (this._settings) {
            for (const key of ['overview-blur', 'overview-blur-strength']) {
                this._overviewSettingsIds.push(
                    this._settings.connect(`changed::${key}`,
                        () => this._onOverviewPrefsChanged()));
            }
        }

        this._setupOverviewBackdrop();

        if (!Main.sessionMode.isLocked)
            this._reloadBackgrounds();
    }

    _readOverviewPrefs() {
        this._overviewBlur = this._settings?.get_boolean('overview-blur') ?? true;
        this._overviewStrength = this._settings?.get_int('overview-blur-strength') ?? 30;
    }

    _onOverviewPrefsChanged() {
        this._readOverviewPrefs();
        for (const entry of this._overviewClones || [])
            this._applyOverviewBlur(entry.blurController);
        try { this._syncOverviewBackdrop(); } catch (_e) {}
    }

    _applyOverviewBlur(controller) {
        const enabled = this._overviewBlur && this._overviewStrength > 0;
        controller?.setState(enabled, enabled, this._overviewStrength);
    }

    disable() {
        for (const id of this._overviewSettingsIds || []) {
            try { this._settings?.disconnect(id); } catch (_e) {}
        }
        this._overviewSettingsIds = [];
        this._injection.clear();
        const actors = [...this._wallpaperActors];
        this._wallpaperActors.clear();
        this._desktopPresentation.clear();
        for (const a of actors) {
            // Skip LiveWallpapers GNOME already destroyed (their on_destroy
            // ran and cleaned up); only destroy the still-live ones, so we
            // never touch an already-disposed object at teardown.
            let alreadyGone = false;
            try { alreadyGone = !!a._wwDestroyed; } catch (_e) { alreadyGone = true; }
            if (alreadyGone)
                continue;
            try { a.destroy(); } catch (_e) {}
        }
        this._teardownOverviewBackdrop();
        if (!Main.sessionMode.isLocked)
            this._reloadBackgrounds();
    }

    // --- Overview backdrop -------------------------------------------------
    // The overview is a component layered over the workspaces, so its own
    // background (the dark margin ring around the rounded workspace previews,
    // normally the stage-bottom SystemBackground) should read as a BLURRED
    // wallpaper — while the workspace previews themselves stay sharp, since
    // the wallpaper is a per-workspace concept. We drop a monitor-filling,
    // blurred renderer clone at the bottom of overviewGroup for those
    // margins; the opaque WorkspaceBackgrounds (carrying the sharp live
    // wallpaper via LiveWallpaper) sit above it and cover the center.
    // overviewGroup is hidden while the overview is closed, so these clones
    // only paint while it's open.
    _setupOverviewBackdrop() {
        try {
            this._overviewClones = [];
            this._overviewGroup = Main.layoutManager.overviewGroup;
            this._overviewBackdropPoll = GLib.timeout_add(
                GLib.PRIORITY_DEFAULT, 1000, () => {
                    try { this._syncOverviewBackdrop(); } catch (_e) {}
                    return GLib.SOURCE_CONTINUE;
                });
            try { this._syncOverviewBackdrop(); } catch (_e) {}
            // Keep our clones correctly stacked: just above blur-my-shell's
            // overview background group if present (so our wallpaper shows
            // over its static blur), otherwise at the very bottom. Re-evaluate
            // on any structural change, because blur-my-shell re-inserts its
            // own group at index 0 whenever another child is added.
            const reposition = () => {
                try { this._repositionOverviewBackdrop(); } catch (_e) {}
            };
            this._overviewChildAdded = this._overviewGroup.connect('child-added', reposition);
            this._overviewChildRemoved = this._overviewGroup.connect('child-removed', reposition);
        } catch (_e) {}
    }

    _bmsOverviewGroup() {
        try {
            return this._overviewGroup.get_children().find(
                c => c?.get_name?.() === 'bms-overview-backgroundgroup') ?? null;
        } catch (_e) {
            return null;
        }
    }

    _repositionOverviewBackdrop() {
        const group = this._overviewGroup;
        const bms = this._bmsOverviewGroup();
        for (const entry of this._overviewClones) {
            if (entry.clone.get_parent() !== group)
                continue;
            try {
                if (bms)
                    group.set_child_above_sibling(entry.clone, bms);
                else
                    group.set_child_below_sibling(entry.clone, null);
            } catch (_e) {}
        }
    }

    _invalidateOverviewClones() {
        for (const entry of [...(this._overviewClones || [])]) {
            entry.blurController?.destroy();
            try { this._overviewGroup?.remove_child(entry.clone); } catch (_e) {}
            try { entry.clone.destroy(); } catch (_e) {}
        }
        this._overviewClones = [];
    }

    _syncOverviewBackdrop() {
        const group = this._overviewGroup;

        const actors = global.get_window_actors(false);
        const renderers = actors.filter(a =>
            a?.meta_window?.title?.includes(APPLICATION_ID) &&
            Wallpaper.rendererPresentationReady(a));
        const currentSources = new Set(renderers);
        const nMonitors = global.display.get_n_monitors();

        for (let m = 0; m < nMonitors; m++) {
            const src = renderers.find(a => a.meta_window.get_monitor() === m);
            if (!src || src.width <= 0)
                continue;
            const geom = global.display.get_monitor_geometry(m);
            let entry = this._overviewClones.find(c => c.monitor === m);
            if (!entry || entry.clone.source !== src) {
                if (entry) {
                    entry.blurController?.destroy();
                    try { group.remove_child(entry.clone); } catch (_e2) {}
                    try { entry.clone.destroy(); } catch (_e2) {}
                    this._overviewClones = this._overviewClones.filter(c => c !== entry);
                }
                const clone = new Clutter.Clone({
                    source: src,
                    opacity: 0,
                });
                const blurController = new BlurController(
                    clone, 'waywallen-overview-blur');
                this._applyOverviewBlur(blurController);
                group.add_child(clone);
                clone.ease({
                    opacity: 255, duration: 400,
                    mode: Clutter.AnimationMode.EASE_OUT_QUAD,
                });
                entry = {clone, monitor: m, blurController};
                this._overviewClones.push(entry);
            }
            const sx = geom.width / src.width;
            const sy = geom.height / src.height;
            entry.clone.set_position(geom.x, geom.y);
            entry.clone.set_scale(sx, sy);
        }

        // prune clones whose renderer disappeared (e.g. it restarted)
        for (const entry of [...this._overviewClones]) {
            if (!currentSources.has(entry.clone.source)) {
                entry.blurController?.destroy();
                try { group.remove_child(entry.clone); } catch (_e) {}
                try { entry.clone.destroy(); } catch (_e) {}
                this._overviewClones = this._overviewClones.filter(c => c !== entry);
            }
        }

        this._repositionOverviewBackdrop();
    }

    _teardownOverviewBackdrop() {
        try {
            if (this._overviewBackdropPoll) {
                GLib.source_remove(this._overviewBackdropPoll);
                this._overviewBackdropPoll = 0;
            }
            for (const id of ['_overviewChildAdded', '_overviewChildRemoved']) {
                if (this[id]) {
                    try { this._overviewGroup?.disconnect(this[id]); } catch (_e) {}
                    this[id] = 0;
                }
            }
            for (const entry of [...(this._overviewClones || [])]) {
                entry.blurController?.destroy();
                try { this._overviewGroup?.remove_child(entry.clone); } catch (_e) {}
                try { entry.clone.destroy(); } catch (_e) {}
            }
            this._overviewClones = [];
        } catch (_e) {}
    }

    applyControlFrame(frame) {
        const key = this._geometryKey(frame.geometry);
        if (frame.type === 'reset') {
            this._desktopPresentation.delete(key);
            this._syncDesktopPresentation(key);
            return true;
        }

        if (frame.type === 'connection' || frame.type === 'presentation-snapshot') {
            const current = this._desktopPresentation.get(key);
            if (frame.type === 'presentation-snapshot' && !current)
                return false;
            if (current &&
                (frame.presentation.config.generation <= current.config.generation ||
                 frame.presentation.state.generation <=
                    current.state.generation))
                return false;
            this._desktopPresentation.set(key, frame.presentation);
            this._syncDesktopPresentation(key);
            return true;
        }

        const current = this._desktopPresentation.get(key);
        const state = frame.state;
        if (!current || state.configGeneration !== current.config.generation ||
            state.generation <= current.state.generation ||
            (current.config.pauseEffect.kind === PauseEffectKind.NONE &&
             state.pauseEffect.active))
            return false;
        current.state = state;
        this._syncDesktopPresentation(key);
        return true;
    }

    resetPresentation() {
        this._desktopPresentation.clear();
        for (const actor of this._wallpaperActors) {
            if (actor.role === Wallpaper.WallpaperRole.Desktop)
                actor.setPresentation(null);
        }
    }

    setRendererAvailable(available) {
        if (this._rendererAvailable === available)
            return;
        this._rendererAvailable = available;
        for (const actor of this._wallpaperActors) {
            try { actor.setRendererAvailable(available); } catch (_e) {}
        }
    }

    setRendererLauncher(launcher) {
        this._rendererLauncher = launcher;
        for (const actor of this._wallpaperActors) {
            try { actor.setRendererLauncher(launcher); } catch (_e) {}
        }
    }

    _geometryKey(geometry) {
        return `${geometry.x}:${geometry.y}:${geometry.width}:${geometry.height}`;
    }

    _actorGeometryKey(actor) {
        const geometry = global.display.get_monitor_geometry(actor.monitorIndex);
        return this._geometryKey(geometry);
    }

    _applyPresentationToActor(actor) {
        let presentation = null;
        try {
            presentation = this._desktopPresentation.get(this._actorGeometryKey(actor)) ?? null;
        } catch (_e) {}
        actor.setPresentation(presentation);
    }

    _syncDesktopPresentation(key) {
        for (const actor of this._wallpaperActors) {
            if (actor.role !== Wallpaper.WallpaperRole.Desktop)
                continue;
            try {
                if (this._actorGeometryKey(actor) === key)
                    this._applyPresentationToActor(actor);
            } catch (_e) {}
        }
    }

    _reloadBackgrounds() {
        Main.layoutManager._updateBackgrounds();
        if (Main.screenShield?._dialog?._updateBackgrounds)
            Main.screenShield._dialog._updateBackgrounds();
        try {
            Main.overview._overview._controls
                ._workspacesDisplay._updateWorkspacesViews();
        } catch (_e) {
            // Other extensions may have already torn down workspace views.
        }
    }
}
