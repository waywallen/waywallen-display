import Shell from 'gi://Shell';

export class BlurController {
    constructor(actor, name) {
        this._actor = actor;
        this._name = name;
        this._effect = null;
        this._configured = false;
        this._active = false;
        this._radius = 30;
    }

    setState(configured, active, radius) {
        this._configured = configured;
        this._active = configured && active;
        this._radius = radius;
        if (!this._configured) {
            this._unload();
            return;
        }
        this._ensureLoaded();
        this._effect.radius = this._radius;
        this._effect.set_enabled(this._active);
    }

    _ensureLoaded() {
        if (this._effect || !this._actor)
            return;
        this._effect = new Shell.BlurEffect({
            mode: Shell.BlurMode.ACTOR,
            radius: this._radius,
        });
        this._actor.add_effect_with_name(this._name, this._effect);
        this._effect.set_enabled(this._active);
    }

    _unload() {
        if (!this._effect)
            return;
        try { this._actor?.remove_effect(this._effect); } catch (_e) {}
        this._effect = null;
    }

    destroy() {
        this._unload();
        this._actor = null;
    }
}
