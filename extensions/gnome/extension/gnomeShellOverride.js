import Meta from 'gi://Meta';
import Shell from 'gi://Shell';

import {InjectionManager} from 'resource:///org/gnome/shell/extensions/extension.js';
import * as Background from 'resource:///org/gnome/shell/ui/background.js';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as Workspace from 'resource:///org/gnome/shell/ui/workspace.js';
import * as WorkspaceThumbnail from 'resource:///org/gnome/shell/ui/workspaceThumbnail.js';

import * as Wallpaper from './wallpaper.js';

const APPLICATION_ID = Wallpaper.APPLICATION_ID;

export class GnomeShellOverride {
    constructor() {
        this._injection = new InjectionManager();
        this._wallpaperActors = new Set();
    }

    enable() {
        const self = this;

        this._injection.overrideMethod(
            Background.BackgroundManager.prototype,
            '_createBackgroundActor',
            originalMethod => function () {
                const backgroundActor = originalMethod.call(this);
                this.waywallenActor = new Wallpaper.LiveWallpaper(backgroundActor);
                self._wallpaperActors.add(this.waywallenActor);
                this.waywallenActor.connect('destroy', a =>
                    self._wallpaperActors.delete(a));
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

        if (!Main.sessionMode.isLocked)
            this._reloadBackgrounds();
    }

    disable() {
        this._injection.clear();
        this._wallpaperActors.forEach(a => a.destroy());
        this._wallpaperActors.clear();
        if (!Main.sessionMode.isLocked)
            this._reloadBackgrounds();
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
