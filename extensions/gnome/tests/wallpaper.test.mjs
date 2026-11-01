// Run with: node --test extensions/gnome/tests/wallpaper.test.mjs
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import test from 'node:test';
import vm from 'node:vm';

class Actor {
    handlers = new Map();
    children = new Set();
    effects = new Set();
    nextId = 1;
    disposed = false;
    constructor(...args) { this._init(...args); }
    _init(props) { Object.assign(this, props); }
    connect(name, callback) {
        assert.ok(!this.disposed);
        const id = this.nextId++;
        this.handlers.set(id, {name, callback});
        return id;
    }
    disconnect(id) { assert.ok(this.handlers.delete(id)); }
    emit(name) {
        for (const [id, handler] of [...this.handlers]) {
            if (this.handlers.has(id) && handler.name === name)
                handler.callback(this);
        }
    }
    add_child(child) { this.children.add(child); child.parent = this; }
    add_effect(effect) { this.effects.add(effect); effect.actor = this; }
    remove_effect(effect) { assert.ok(this.effects.delete(effect)); effect.actor = null; }
    vfunc_allocate(box) { this.allocation = box; }
    queue_relayout() { this.relayouts = (this.relayouts ?? 0) + 1; }
    set_position(x, y) { Object.assign(this, {x, y}); }
    set_scale(scale_x, scale_y) { Object.assign(this, {scale_x, scale_y}); }
    get_allocation_box() { return this.allocation; }
    get_paint_volume() { return this.paintVolume ?? null; }
    get_resource_scale() { return this.resourceScale ?? 1; }
    set_style() {}
    ease(props) { assert.ok(!this.disposed); this.opacity = props.opacity; }
    remove_all_transitions() { assert.ok(!this.disposed); }
    destroy() {
        assert.ok(!this.disposed, 'actor destroyed twice');
        this.destroying = true;
        for (const [id, handler] of [...this.handlers]) {
            if (this.handlers.has(id) && handler.name === 'destroy')
                handler.callback(this);
        }
        // A class closure override replaces Clutter's default child destruction.
        if (this.on_destroy)
            this.on_destroy();
        else
            for (const child of [...this.children]) child.destroy();
        this.parent?.children.delete(this);
        this.handlers.clear();
        this.disposed = true;
    }
}

class ShaderEffect {
    uniforms = new Map();
    get_actor() { return this.actor; }
    get_texture() { return this.texture; }
    get_uniform_location(name) { return name; }
    set_uniform_float(name, count, values) {
        assert.equal(values.length, count);
        this.uniforms.set(name, Array.from(values));
    }
}

function allocation(width, height) {
    return {x1: 0, y1: 0, x2: width, y2: height,
        get_width: () => width, get_height: () => height};
}

function setup() {
    const timers = new Map();
    let nextTimer = 1;
    const windows = [];
    const actors = new Set();
    const monitor = {x: 1920, y: 0, width: 1920, height: 1080};
    const work = {x: 1960, y: 32, width: 1880, height: 1048};
    const GLib = {
        PRIORITY_DEFAULT: 0, SOURCE_REMOVE: false,
        timeout_add(_priority, _delay, callback) {
            const id = nextTimer++;
            timers.set(id, callback);
            return id;
        },
        source_remove(id) { assert.ok(timers.delete(id)); },
    };
    const source = readFileSync(new URL('../extension/wallpaper.js', import.meta.url), 'utf8')
        .replace(/^import .*;\n/gm, '')
        .replace(/^export /gm, '');
    const Wallpaper = vm.runInNewContext(`${source}\n({LiveWallpaper, WallpaperRole, APPLICATION_ID})`, {
        St: {Widget: Actor}, GObject: {registerClass: klass => klass}, GLib,
        Clutter: {Clone: Actor, FixedLayout: class {}, BinLayout: class {}, AnimationMode: {}},
        Shell: {GLSLEffect: ShaderEffect},
        Main: {layoutManager: {monitors: [monitor], getWorkAreaForMonitor: () => work}},
        BlurController: class { setState() {} destroy() {} },
        PauseEffectKind: {BLUR: 'blur'},
        global: {display: {
            get_n_monitors: () => 1,
            list_all_windows: () => windows.filter(w => !w.disposed).map(w => w.meta_window),
        }},
    });
    function renderer() {
        const actor = new Actor({meta_window: {
            title: `@${Wallpaper.APPLICATION_ID}!{"presentationReady":true}`,
            get_monitor: () => 0,
        }, width: 1920, height: 1080});
        actor.meta_window.get_compositor_private = () => actor;
        windows.push(actor);
        return {actor, launcher: {ownsWindow: window => window === actor.meta_window}};
    }
    function wallpaper(launcher, available = true, role = Wallpaper.WallpaperRole.Desktop) {
        const background = new Actor({monitor: 0});
        const content = new Actor({brightness: 0.5, vignette: false, rounded_clip_radius: 0});
        Object.defineProperty(background, 'content', {get() {
            assert.ok(!background.destroying && !background.disposed, 'read disposing background');
            return content;
        }});
        const actor = new Wallpaper.LiveWallpaper(background, role, available, launcher,
            destroyed => actors.delete(destroyed));
        actors.add(actor);
        return {actor, background, content};
    }
    return {renderer, wallpaper, timers, actors, flush() {
        for (const [id, callback] of [...timers]) {
            timers.delete(id);
            assert.equal(callback(), false);
        }
    }};
}

test('parent destruction unregisters wallpaper and preserves default child destruction', () => {
    const s = setup();
    const r = s.renderer();
    const {actor, background, content} = s.wallpaper(r.launcher);
    const clone = actor._cloneActor;
    const extraChild = new Actor();
    actor.add_child(extraChild);
    background.destroy();
    assert.equal(s.actors.size, 0);
    assert.ok(actor.disposed && clone.disposed && extraChild.disposed);
    assert.equal(r.actor.handlers.size, 0);
    assert.equal(content.handlers.size, 0);
    assert.equal(s.timers.size, 0);
    actor.setRendererAvailable(true);
    actor.setRendererLauncher(null);
    actor.setPresentation(null);
    r.actor.destroy();
});

test('explicit destruction restores backdrop and disconnects background and polling', () => {
    const s = setup();
    const {actor, background, content} = s.wallpaper(null, false);
    assert.equal(content.brightness, 0);
    actor.setRendererAvailable(true);
    assert.equal(s.timers.size, 1);
    const pending = [...s.timers.values()][0];
    actor.destroy();
    assert.equal(s.actors.size, 0);
    assert.equal(s.timers.size, 0);
    assert.equal(background.handlers.size, 0);
    assert.equal(content.brightness, 0.5);
    assert.equal(content.vignette, false);
    assert.equal(content.handlers.size, 0);
    pending();
    assert.equal(s.timers.size, 0);
    background.destroy();
});

test('daemon restart attaches only the new renderer even while the old window survives', () => {
    const s = setup();
    let r = s.renderer();
    const {actor, background} = s.wallpaper(r.launcher);
    for (let i = 0; i < 3; i++) {
        const old = r;
        const clone = actor._cloneActor;
        actor.setRendererAvailable(false);
        actor.setRendererLauncher(null);
        assert.ok(clone.disposed);
        assert.equal(old.actor.handlers.size, 0);
        r = s.renderer();
        actor.setRendererLauncher(r.launcher);
        actor.setRendererAvailable(true);
        assert.equal(actor._cloneActor.source, r.actor);
        old.actor.destroy();
        assert.equal(actor._cloneActor.source, r.actor);
    }
    background.destroy();
});

test('source destruction polls for replacement, and parent destruction cancels polling', () => {
    const s = setup();
    const r = s.renderer();
    const {actor, background} = s.wallpaper(r.launcher);
    r.actor.destroy();
    assert.equal(actor._cloneActor, null);
    assert.equal(s.timers.size, 1);
    s.flush();
    assert.equal(s.timers.size, 1);
    background.destroy();
    assert.equal(s.timers.size, 0);
    assert.equal(s.actors.size, 0);
});

test('overview reset restores the backdrop and reconnect dims it again', () => {
    const s = setup();
    const r = s.renderer();
    const {actor, background, content} = s.wallpaper(r.launcher, true, 'other');
    assert.equal(content.brightness, 0);
    assert.equal(content.vignette, true);
    actor.setRendererAvailable(false);
    assert.equal(content.brightness, 0.5);
    assert.equal(content.vignette, false);
    actor.setRendererAvailable(true);
    assert.equal(actor._cloneActor.source, r.actor);
    assert.equal(content.brightness, 0);
    assert.equal(content.vignette, true);
    background.destroy();
});

test('unexpected clone destruction disconnects source before attaching again', () => {
    const s = setup();
    const r = s.renderer();
    const {actor, background} = s.wallpaper(r.launcher);
    actor._cloneActor.destroy();
    assert.equal(r.actor.handlers.size, 0);
    assert.equal(actor._cloneActor, null);
    s.flush();
    assert.equal(actor._cloneActor.source, r.actor);
    assert.equal(r.actor.handlers.size, 1);
    background.destroy();
});

test('launcher replacement cannot retain the previous renderer clone', () => {
    const s = setup();
    const old = s.renderer();
    const {actor, background} = s.wallpaper(old.launcher);
    const clone = actor._cloneActor;
    const next = s.renderer();
    actor.setRendererLauncher(next.launcher);
    assert.ok(clone.disposed);
    assert.equal(old.actor.handlers.size, 0);
    assert.equal(actor._cloneActor.source, next.actor);
    background.destroy();
});

test('overview clip follows work area, preview size and animated radius', () => {
    const s = setup();
    const r = s.renderer();
    const {actor, background, content} = s.wallpaper(r.launcher, true, 'other');
    actor.vfunc_allocate(allocation(1920, 1080));
    assert.equal(actor.effects.size, 0);

    content.rounded_clip_radius = 30;
    content.emit('notify::rounded-clip-radius');
    assert.equal(actor.relayouts, 1);
    actor.vfunc_allocate(allocation(960, 540));
    const effect = actor._clipEffect;
    assert.ok(actor.effects.has(effect));
    assert.deepEqual(Array.from(effect._bounds), [20, 16, 960, 540]);
    assert.equal(effect._radius, 15);
    assert.equal(actor._cloneActor.scale_x, 0.5);
    assert.equal(actor._cloneActor.scale_y, 0.5);

    actor.vfunc_allocate(allocation(480, 270));
    assert.equal(actor._clipEffect, effect);
    assert.deepEqual(Array.from(effect._bounds), [10, 8, 480, 270]);
    assert.equal(effect._radius, 7.5);

    content.rounded_clip_radius = 0;
    content.emit('notify::rounded-clip-radius');
    actor.vfunc_allocate(allocation(1920, 1080));
    assert.equal(actor._clipEffect, null);
    assert.equal(actor.effects.size, 0);
    background.destroy();
    assert.equal(content.handlers.size, 0);
});

test('clip uniforms account for offscreen offset and HiDPI changes', () => {
    const s = setup();
    const r = s.renderer();
    const {actor, background, content} = s.wallpaper(r.launcher, true, 'other');
    content.rounded_clip_radius = 30;
    actor.vfunc_allocate(allocation(960, 540));
    const effect = actor._clipEffect;
    effect.texture = {get_width: () => 964, get_height: () => 544};
    effect._uploadUniforms();
    assert.deepEqual(effect.uniforms.get('bounds'), [22, 18, 962, 542]);
    assert.deepEqual(effect.uniforms.get('clip_radius'), [15]);
    assert.deepEqual(effect.uniforms.get('pixel_step'), [1 / 964, 1 / 544]);

    actor.resourceScale = 2;
    effect.texture = {get_width: () => 1928, get_height: () => 1088};
    effect._uploadUniforms();
    assert.deepEqual(effect.uniforms.get('bounds'), [44, 36, 1924, 1084]);
    assert.deepEqual(effect.uniforms.get('clip_radius'), [30]);
    assert.deepEqual(effect.uniforms.get('pixel_step'), [1 / 1928, 1 / 1088]);

    actor.paintVolume = {get_origin: () => ({x: -10, y: -6}),
        get_width: () => 980, get_height: () => 552};
    effect._uploadUniforms();
    assert.deepEqual(effect.uniforms.get('bounds'), [64, 48, 1944, 1096]);
    background.destroy();
    assert.equal(actor._clipEffect, null);
    assert.equal(actor.effects.size, 0);
    assert.equal(content.handlers.size, 0);
});
