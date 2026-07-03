//! waywallen-layer-shell — Wayland layer-shell wallpaper client.
//!
//! Connects to a Wayland compositor that supports `zwlr_layer_shell_v1`
//! (Hyprland, Sway, Niri, River, …) and registers each output as a
//! display with the daemon over the waywallen-display UDS protocol.

mod watcher;

use std::collections::HashMap;
use std::ffi::{c_void, CStr, CString};
use std::os::fd::{AsFd, FromRawFd, OwnedFd};
use std::os::unix::fs::FileExt;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::Duration;

use anyhow::{anyhow, bail, Context, Result};
use md5::{Digest, Md5};
use wayland_client::globals::{registry_queue_init, GlobalListContents};
use wayland_client::protocol::{
    wl_buffer::WlBuffer,
    wl_callback::{self, WlCallback},
    wl_compositor::WlCompositor,
    wl_output::{self, Transform, WlOutput},
    wl_pointer::{self, ButtonState, WlPointer},
    wl_registry::WlRegistry,
    wl_seat::{self, WlSeat},
    wl_surface::WlSurface,
};
use wayland_client::{Connection, Dispatch, Proxy, QueueHandle};
use wayland_protocols::wp::fractional_scale::v1::client::{
    wp_fractional_scale_manager_v1::{self, WpFractionalScaleManagerV1},
    wp_fractional_scale_v1::{self, WpFractionalScaleV1},
};
use wayland_protocols::wp::linux_dmabuf::zv1::client::{
    zwp_linux_buffer_params_v1::{self, ZwpLinuxBufferParamsV1},
    zwp_linux_dmabuf_feedback_v1::{self, ZwpLinuxDmabufFeedbackV1},
    zwp_linux_dmabuf_v1::{self, ZwpLinuxDmabufV1},
};
use wayland_protocols::wp::viewporter::client::{
    wp_viewport::{self, WpViewport},
    wp_viewporter::{self, WpViewporter},
};
use wayland_protocols_wlr::layer_shell::v1::client::{
    zwlr_layer_shell_v1::{self, Layer, ZwlrLayerShellV1},
    zwlr_layer_surface_v1::{self, Anchor, KeyboardInteractivity, ZwlrLayerSurfaceV1},
};
use waywallen_display as sys;

fn default_socket_path() -> PathBuf {
    let runtime = std::env::var_os("XDG_RUNTIME_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("/tmp"));
    runtime.join("waywallen").join("display.sock")
}

// ---------------------------------------------------------------------------
// Per-output state
// ---------------------------------------------------------------------------

pub struct OutputBinding {
    display_name: String,
    instance_id: String,
    surface: WlSurface,
    dmabuf: ZwpLinuxDmabufV1,
    conn: Connection,
    qh: QueueHandle<App>,
    output_name: u32,
    configured_size: Mutex<Option<(u32, u32)>>,
    logical_size: Mutex<Option<(u32, u32)>>,
    scale: std::sync::atomic::AtomicI32,
    fractional_scale_120: AtomicU32,
    refresh_mhz: AtomicU32,
    viewport: Option<WpViewport>,
    closed: AtomicBool,
    display: Mutex<Option<DisplayPtr>>,
    frame_pending: AtomicBool,
    registered: AtomicBool,
    last_pushed_size: Mutex<Option<(u32, u32)>>,
    layer_buffer: Mutex<Option<LayerBuffer>>,
    config: Mutex<FrameConfig>,
    window_flags: AtomicU32,
}

impl OutputBinding {
    pub fn display_name(&self) -> &str {
        &self.display_name
    }
    pub fn window_flags(&self) -> &AtomicU32 {
        &self.window_flags
    }
    pub fn is_registered(&self) -> bool {
        self.registered.load(Ordering::SeqCst)
    }
    pub fn with_display<F>(&self, f: F) -> Option<i32>
    where
        F: FnOnce(*mut sys::waywallen_display_t) -> i32,
    {
        let guard = self.display.lock().unwrap();
        guard.as_ref().map(|d| f(d.0))
    }
}

#[derive(Copy, Clone)]
struct DisplayPtr(*mut sys::waywallen_display_t);

unsafe impl Send for DisplayPtr {}
unsafe impl Sync for DisplayPtr {}

struct LayerBuffer {
    wl_buffer: WlBuffer,
    width: u32,
    height: u32,
}

impl Drop for LayerBuffer {
    fn drop(&mut self) {
        self.wl_buffer.destroy();
    }
}

#[derive(Clone, Copy)]
struct FrameConfig {
    source: Option<(f32, f32, f32, f32)>,
    transform: u32,
    transform_dirty: bool,
}

impl Default for FrameConfig {
    fn default() -> Self {
        Self {
            source: None,
            transform: 0,
            transform_dirty: true,
        }
    }
}

struct OutputEntry {
    wl_output: WlOutput,
    surface: Option<WlSurface>,
    layer_surface: Option<ZwlrLayerSurfaceV1>,
    viewport: Option<WpViewport>,
    binding: Option<Arc<OutputBinding>>,
    worker_started: bool,
    scale: i32,
    fractional_scale: Option<WpFractionalScaleV1>,
    fractional_scale_120: u32,
    refresh_mhz: u32,
    output_name_str: Option<String>,
    output_description: Option<String>,
    output_make: Option<String>,
    output_model: Option<String>,
}

struct App {
    compositor: Option<WlCompositor>,
    layer_shell: Option<ZwlrLayerShellV1>,
    dmabuf: Option<ZwpLinuxDmabufV1>,
    viewporter: Option<WpViewporter>,
    fractional_scale_mgr: Option<WpFractionalScaleManagerV1>,
    dmabuf_feedback: Option<ZwpLinuxDmabufFeedbackV1>,
    compositor_drm_major: u32,
    compositor_drm_minor: u32,
    dmabuf_format_table: Vec<(u32, u64)>,
    outputs: HashMap<u32, OutputEntry>,
    uds_sock: PathBuf,
    name_prefix: String,
    pointers: HashMap<u32, PointerCtx>,
    binding_registry: watcher::BindingRegistry,
}

struct PointerCtx {
    pointer: WlPointer,
    focus_output: Option<u32>,
    last_x: f64,
    last_y: f64,
    axis_source: u32,
}

impl App {
    fn new(uds_sock: PathBuf, name_prefix: String) -> Self {
        Self {
            compositor: None,
            layer_shell: None,
            dmabuf: None,
            viewporter: None,
            fractional_scale_mgr: None,
            dmabuf_feedback: None,
            compositor_drm_major: 0,
            compositor_drm_minor: 0,
            dmabuf_format_table: Vec::new(),
            outputs: HashMap::new(),
            uds_sock,
            name_prefix,
            pointers: HashMap::new(),
            binding_registry: watcher::new_registry(),
        }
    }

    fn bring_up_surface(&mut self, output_name: u32, qh: &QueueHandle<App>) {
        let Some(entry) = self.outputs.get_mut(&output_name) else {
            return;
        };
        if entry.surface.is_some() {
            return;
        }
        let (Some(comp), Some(shell)) = (self.compositor.as_ref(), self.layer_shell.as_ref())
        else {
            return;
        };
        let surface = comp.create_surface(qh, output_name);
        let layer_surface = shell.get_layer_surface(
            &surface,
            Some(&entry.wl_output),
            Layer::Background,
            "waywallen-wallpaper".to_string(),
            qh,
            output_name,
        );
        layer_surface.set_anchor(Anchor::Top | Anchor::Bottom | Anchor::Left | Anchor::Right);
        layer_surface.set_exclusive_zone(-1);
        layer_surface.set_keyboard_interactivity(KeyboardInteractivity::None);
        layer_surface.set_size(0, 0);
        let viewport = self
            .viewporter
            .as_ref()
            .map(|vp| vp.get_viewport(&surface, qh, output_name));
        let fractional_scale = self
            .fractional_scale_mgr
            .as_ref()
            .map(|m| m.get_fractional_scale(&surface, qh, output_name));
        surface.commit();
        entry.surface = Some(surface);
        entry.layer_surface = Some(layer_surface);
        entry.viewport = viewport;
        entry.fractional_scale = fractional_scale;
        log::info!("output {output_name}: layer_surface committed, waiting for configure");
    }

    fn maybe_spawn_worker(&mut self, output_name: u32) {
        let Some(entry) = self.outputs.get_mut(&output_name) else {
            return;
        };
        if entry.worker_started {
            return;
        }
        let Some(binding) = entry.binding.as_ref() else {
            return;
        };
        if binding.configured_size.lock().unwrap().is_none() {
            return;
        }
        entry.worker_started = true;
        let binding = Arc::clone(binding);
        let sock = self.uds_sock.clone();
        log::info!(
            "output {output_name}: spawning UDS worker ('{}')",
            binding.display_name
        );
        thread::spawn(move || uds_worker_loop(sock, binding));
    }
}

// ---------------------------------------------------------------------------
// Dispatch impls
// ---------------------------------------------------------------------------

impl Dispatch<WlRegistry, GlobalListContents> for App {
    fn event(
        state: &mut Self,
        registry: &WlRegistry,
        event: wayland_client::protocol::wl_registry::Event,
        _data: &GlobalListContents,
        _conn: &Connection,
        qh: &QueueHandle<Self>,
    ) {
        use wayland_client::protocol::wl_registry::Event;
        match event {
            Event::Global {
                name,
                interface,
                version,
            } => {
                if interface == "wl_output" {
                    if state.outputs.contains_key(&name) {
                        return;
                    }
                    let wl_output = registry.bind::<WlOutput, _, _>(name, version.min(4), qh, name);
                    state.outputs.insert(
                        name,
                        OutputEntry {
                            wl_output,
                            surface: None,
                            layer_surface: None,
                            viewport: None,
                            binding: None,
                            worker_started: false,
                            scale: 1,
                            fractional_scale: None,
                            fractional_scale_120: 0,
                            refresh_mhz: 60_000,
                            output_name_str: None,
                            output_description: None,
                            output_make: None,
                            output_model: None,
                        },
                    );
                    log::info!("hot-plug: wl_output name={name} added; bringing up surface");
                    state.bring_up_surface(name, qh);
                } else if interface == "wl_seat" {
                    registry.bind::<WlSeat, _, _>(name, version.min(5), qh, name);
                    log::info!("hot-plug: wl_seat name={name} added");
                }
            }
            Event::GlobalRemove { name } => {
                if let Some(ctx) = state.pointers.remove(&name) {
                    log::info!("hot-unplug: wl_seat name={name} removed");
                    ctx.pointer.release();
                }
                if let Some(entry) = state.outputs.remove(&name) {
                    log::info!("hot-unplug: wl_output name={name} removed");
                    if let Some(binding) = entry.binding.as_ref() {
                        binding.closed.store(true, Ordering::SeqCst);
                        state
                            .binding_registry
                            .lock()
                            .unwrap()
                            .remove(binding.display_name());
                    }
                    drop(entry);
                }
            }
            _ => {}
        }
    }
}

impl Dispatch<WlCompositor, ()> for App {
    fn event(
        _state: &mut Self,
        _p: &WlCompositor,
        _e: wayland_client::protocol::wl_compositor::Event,
        _data: &(),
        _conn: &Connection,
        _qh: &QueueHandle<Self>,
    ) {
    }
}

impl Dispatch<WlSurface, u32> for App {
    fn event(
        _state: &mut Self,
        _p: &WlSurface,
        _e: wayland_client::protocol::wl_surface::Event,
        _data: &u32,
        _conn: &Connection,
        _qh: &QueueHandle<Self>,
    ) {
    }
}

impl Dispatch<WlSeat, u32> for App {
    fn event(
        state: &mut Self,
        seat: &WlSeat,
        event: wl_seat::Event,
        data: &u32,
        _conn: &Connection,
        qh: &QueueHandle<Self>,
    ) {
        let seat_name = *data;
        match event {
            wl_seat::Event::Capabilities { capabilities } => {
                let has_pointer = match capabilities {
                    wayland_client::WEnum::Value(c) => c.contains(wl_seat::Capability::Pointer),
                    _ => false,
                };
                let already = state.pointers.contains_key(&seat_name);
                if has_pointer && !already {
                    let pointer = seat.get_pointer(qh, seat_name);
                    state.pointers.insert(
                        seat_name,
                        PointerCtx {
                            pointer,
                            focus_output: None,
                            last_x: 0.0,
                            last_y: 0.0,
                            axis_source: 0,
                        },
                    );
                    log::info!("wl_seat name={seat_name} acquired pointer");
                } else if !has_pointer && already {
                    if let Some(ctx) = state.pointers.remove(&seat_name) {
                        ctx.pointer.release();
                    }
                    log::info!("wl_seat name={seat_name} lost pointer capability");
                }
            }
            _ => {}
        }
    }
}

impl Dispatch<WlPointer, u32> for App {
    fn event(
        state: &mut Self,
        _p: &WlPointer,
        event: wl_pointer::Event,
        data: &u32,
        _conn: &Connection,
        _qh: &QueueHandle<Self>,
    ) {
        let seat_name = *data;
        match event {
            wl_pointer::Event::Enter {
                surface,
                surface_x,
                surface_y,
                ..
            } => {
                let output_name = match surface.data::<u32>() {
                    Some(n) => *n,
                    None => return,
                };
                if let Some(ctx) = state.pointers.get_mut(&seat_name) {
                    ctx.focus_output = Some(output_name);
                    ctx.last_x = surface_x;
                    ctx.last_y = surface_y;
                }
            }
            wl_pointer::Event::Leave { .. } => {
                if let Some(ctx) = state.pointers.get_mut(&seat_name) {
                    ctx.focus_output = None;
                }
            }
            wl_pointer::Event::Motion {
                time,
                surface_x,
                surface_y,
            } => {
                let (output_name, lx, ly) = {
                    let Some(ctx) = state.pointers.get_mut(&seat_name) else {
                        return;
                    };
                    ctx.last_x = surface_x;
                    ctx.last_y = surface_y;
                    let Some(out) = ctx.focus_output else { return };
                    (out, surface_x, surface_y)
                };
                let (x, y) = logical_to_physical(state, output_name, lx, ly);
                send_pointer_motion(state, output_name, x, y, ms_to_us(time));
            }
            wl_pointer::Event::Button {
                time,
                button,
                state: bstate,
                ..
            } => {
                let (output_name, lx, ly) = {
                    let Some(ctx) = state.pointers.get(&seat_name) else {
                        return;
                    };
                    let Some(out) = ctx.focus_output else { return };
                    (out, ctx.last_x, ctx.last_y)
                };
                let (x, y) = logical_to_physical(state, output_name, lx, ly);
                let state_u32 = match bstate {
                    wayland_client::WEnum::Value(ButtonState::Pressed) => 1,
                    wayland_client::WEnum::Value(ButtonState::Released) => 0,
                    _ => return,
                };
                send_pointer_button(state, output_name, x, y, button, state_u32, ms_to_us(time));
            }
            wl_pointer::Event::Axis { time, axis, value } => {
                let (output_name, lx, ly, src) = {
                    let Some(ctx) = state.pointers.get(&seat_name) else {
                        return;
                    };
                    let Some(out) = ctx.focus_output else { return };
                    (out, ctx.last_x, ctx.last_y, ctx.axis_source)
                };
                let (x, y) = logical_to_physical(state, output_name, lx, ly);
                let delta = (value as f32) / 10.0;
                let (dx, dy) = match axis {
                    wayland_client::WEnum::Value(wl_pointer::Axis::HorizontalScroll) => {
                        (delta, 0.0)
                    }
                    wayland_client::WEnum::Value(wl_pointer::Axis::VerticalScroll) => (0.0, delta),
                    _ => return,
                };
                send_pointer_axis(state, output_name, x, y, dx, dy, src, ms_to_us(time));
            }
            wl_pointer::Event::AxisSource { axis_source } => {
                if let Some(ctx) = state.pointers.get_mut(&seat_name) {
                    ctx.axis_source = match axis_source {
                        wayland_client::WEnum::Value(wl_pointer::AxisSource::Wheel) => 0,
                        wayland_client::WEnum::Value(wl_pointer::AxisSource::Finger) => 1,
                        wayland_client::WEnum::Value(wl_pointer::AxisSource::Continuous) => 2,
                        _ => 0,
                    };
                }
            }
            _ => {}
        }
    }
}

fn ms_to_us(time_ms: u32) -> u64 {
    (time_ms as u64).saturating_mul(1000)
}

fn output_part(value: Option<&str>) -> &str {
    value.map(str::trim).filter(|s| !s.is_empty()).unwrap_or("")
}

fn output_identity_key(entry: &OutputEntry, output_name: u32) -> String {
    let name = output_part(entry.output_name_str.as_deref());
    let description = output_part(entry.output_description.as_deref());
    let make = output_part(entry.output_make.as_deref());
    let model = output_part(entry.output_model.as_deref());
    if name.is_empty() && description.is_empty() && make.is_empty() && model.is_empty() {
        return format!("global={output_name}");
    }
    format!("name={name}|description={description}|make={make}|model={model}")
}

fn layer_instance_id(entry: &OutputEntry, output_name: u32) -> String {
    let mut hasher = Md5::new();
    hasher.update(output_identity_key(entry, output_name).as_bytes());
    format!("layer-{:x}", hasher.finalize())
}

fn wl_transform(value: u32) -> Transform {
    match value {
        1 => Transform::_90,
        2 => Transform::_180,
        3 => Transform::_270,
        4 => Transform::Flipped,
        5 => Transform::Flipped90,
        6 => Transform::Flipped180,
        7 => Transform::Flipped270,
        _ => Transform::Normal,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn maps_config_transform_to_wayland_transform() {
        assert_eq!(wl_transform(0), Transform::Normal);
        assert_eq!(wl_transform(1), Transform::_90);
        assert_eq!(wl_transform(2), Transform::_180);
        assert_eq!(wl_transform(3), Transform::_270);
        assert_eq!(wl_transform(4), Transform::Flipped);
        assert_eq!(wl_transform(5), Transform::Flipped90);
        assert_eq!(wl_transform(6), Transform::Flipped180);
        assert_eq!(wl_transform(7), Transform::Flipped270);
        assert_eq!(wl_transform(8), Transform::Normal);
    }
}

fn logical_to_physical(state: &App, output_name: u32, lx: f64, ly: f64) -> (f32, f32) {
    let Some(entry) = state.outputs.get(&output_name) else {
        return (lx as f32, ly as f32);
    };
    let Some(binding) = entry.binding.as_ref() else {
        return (lx as f32, ly as f32);
    };
    let frac = binding.fractional_scale_120.load(Ordering::Relaxed);
    let s = if frac > 0 {
        frac as f64 / 120.0
    } else {
        binding.scale.load(Ordering::Relaxed).max(1) as f64
    };
    ((lx * s) as f32, (ly * s) as f32)
}

fn send_pointer_motion(state: &App, output_name: u32, x: f32, y: f32, timestamp_us: u64) {
    let Some(entry) = state.outputs.get(&output_name) else {
        return;
    };
    let Some(binding) = entry.binding.as_ref() else {
        return;
    };
    if !binding.registered.load(Ordering::Relaxed) {
        return;
    }
    let rc = binding.with_display(|d| unsafe {
        sys::waywallen_display_send_pointer_motion(d, x, y, timestamp_us, 0)
    });
    if let Some(rc) = rc {
        if rc < 0 {
            log::debug!(
                "[{}] send pointer_motion failed: {rc}",
                binding.display_name
            );
        }
    }
}

fn send_pointer_button(
    state: &App,
    output_name: u32,
    x: f32,
    y: f32,
    button: u32,
    state_u32: u32,
    timestamp_us: u64,
) {
    let Some(binding) = state
        .outputs
        .get(&output_name)
        .and_then(|e| e.binding.as_ref())
    else {
        return;
    };
    if !binding.registered.load(Ordering::Relaxed) {
        return;
    }
    let button_state = if state_u32 == 1 {
        sys::WAYWALLEN_BUTTON_PRESSED
    } else {
        sys::WAYWALLEN_BUTTON_RELEASED
    };
    let rc = binding.with_display(|d| unsafe {
        sys::waywallen_display_send_pointer_button(d, x, y, button, button_state, timestamp_us, 0)
    });
    if let Some(rc) = rc {
        if rc < 0 {
            log::debug!(
                "[{}] send pointer_button failed: {rc}",
                binding.display_name
            );
        }
    }
}

fn send_pointer_axis(
    state: &App,
    output_name: u32,
    x: f32,
    y: f32,
    delta_x: f32,
    delta_y: f32,
    source: u32,
    timestamp_us: u64,
) {
    let Some(binding) = state
        .outputs
        .get(&output_name)
        .and_then(|e| e.binding.as_ref())
    else {
        return;
    };
    if !binding.registered.load(Ordering::Relaxed) {
        return;
    }
    let source = match source {
        1 => sys::WAYWALLEN_AXIS_FINGER,
        2 => sys::WAYWALLEN_AXIS_CONTINUOUS,
        _ => sys::WAYWALLEN_AXIS_WHEEL,
    };
    let rc = binding.with_display(|d| unsafe {
        sys::waywallen_display_send_pointer_axis(d, x, y, delta_x, delta_y, source, timestamp_us, 0)
    });
    if let Some(rc) = rc {
        if rc < 0 {
            log::debug!("[{}] send pointer_axis failed: {rc}", binding.display_name);
        }
    }
}

impl Dispatch<WlBuffer, (u32, u32)> for App {
    fn event(
        _state: &mut Self,
        buffer: &WlBuffer,
        event: wayland_client::protocol::wl_buffer::Event,
        data: &(u32, u32),
        _conn: &Connection,
        _qh: &QueueHandle<Self>,
    ) {
        let (output_name, buffer_index) = *data;
        if let wayland_client::protocol::wl_buffer::Event::Release = event {
            log::trace!(
                "wl_buffer {} (out={output_name} idx={buffer_index}) released",
                buffer.id()
            );
        }
    }
}

impl Dispatch<WlCallback, u32> for App {
    fn event(
        state: &mut Self,
        _cb: &WlCallback,
        event: wl_callback::Event,
        data: &u32,
        _conn: &Connection,
        _qh: &QueueHandle<Self>,
    ) {
        if let wl_callback::Event::Done { .. } = event {
            let output_name = *data;
            if let Some(binding) = state
                .outputs
                .get(&output_name)
                .and_then(|e| e.binding.as_ref())
            {
                binding.frame_pending.store(false, Ordering::SeqCst);
            }
        }
    }
}

impl Dispatch<WlOutput, u32> for App {
    fn event(
        state: &mut Self,
        _p: &WlOutput,
        event: wl_output::Event,
        data: &u32,
        _conn: &Connection,
        _qh: &QueueHandle<Self>,
    ) {
        let output_name = *data;
        match event {
            wl_output::Event::Scale { factor } => {
                if let Some(entry) = state.outputs.get_mut(&output_name) {
                    entry.scale = factor.max(1);
                    if let Some(binding) = entry.binding.as_ref() {
                        binding.scale.store(factor.max(1), Ordering::SeqCst);
                    }
                }
            }
            wl_output::Event::Name { name } => {
                if let Some(entry) = state.outputs.get_mut(&output_name) {
                    log::info!("output {output_name}: wl_output.name = {name:?}");
                    entry.output_name_str = Some(name);
                }
            }
            wl_output::Event::Description { description } => {
                if let Some(entry) = state.outputs.get_mut(&output_name) {
                    log::info!("output {output_name}: wl_output.description = {description:?}");
                    entry.output_description = Some(description);
                }
            }
            wl_output::Event::Geometry { make, model, .. } => {
                if let Some(entry) = state.outputs.get_mut(&output_name) {
                    log::info!(
                        "output {output_name}: wl_output.geometry make={make:?} model={model:?}"
                    );
                    entry.output_make = Some(make);
                    entry.output_model = Some(model);
                }
            }
            wl_output::Event::Mode { flags, refresh, .. } => {
                let is_current = match flags {
                    wayland_client::WEnum::Value(flags) => flags.contains(wl_output::Mode::Current),
                    _ => false,
                };
                if is_current && refresh > 0 {
                    if let Some(entry) = state.outputs.get_mut(&output_name) {
                        let refresh_mhz = refresh as u32;
                        entry.refresh_mhz = refresh_mhz;
                        if let Some(binding) = entry.binding.as_ref() {
                            binding.refresh_mhz.store(refresh_mhz, Ordering::SeqCst);
                        }
                        log::info!(
                            "output {output_name}: wl_output.mode current refresh={refresh_mhz}mHz"
                        );
                    }
                }
            }
            _ => {}
        }
    }
}

impl Dispatch<ZwpLinuxDmabufFeedbackV1, ()> for App {
    fn event(
        state: &mut Self,
        _p: &ZwpLinuxDmabufFeedbackV1,
        event: zwp_linux_dmabuf_feedback_v1::Event,
        _data: &(),
        _conn: &Connection,
        _qh: &QueueHandle<Self>,
    ) {
        match event {
            zwp_linux_dmabuf_feedback_v1::Event::MainDevice { device } => {
                if device.len() < 8 {
                    log::warn!(
                        "dmabuf_feedback: main_device {} bytes (want >=8); ignoring",
                        device.len()
                    );
                    return;
                }
                let mut buf = [0u8; 8];
                buf.copy_from_slice(&device[..8]);
                let dev = u64::from_ne_bytes(buf);
                let major = (((dev >> 8) & 0xfff) | ((dev >> 32) & !0xfff_u64)) as u32;
                let minor = ((dev & 0xff) | ((dev >> 12) & !0xff_u64)) as u32;
                log::info!(
                    "dmabuf_feedback: main_device dev_t=0x{dev:x} → DRM render-node {major}:{minor}"
                );
                state.compositor_drm_major = major;
                state.compositor_drm_minor = minor;
            }
            zwp_linux_dmabuf_feedback_v1::Event::FormatTable { fd, size } => {
                let size = size as usize;
                let mut bytes = vec![0u8; size];
                let file = std::fs::File::from(fd);
                if let Err(e) = file.read_exact_at(&mut bytes, 0) {
                    log::warn!("dmabuf_feedback: format_table read failed: {e}");
                    return;
                }
                if size % 16 != 0 {
                    log::warn!(
                        "dmabuf_feedback: format_table size={size} is not a multiple of 16; truncating"
                    );
                }
                let entries: Vec<(u32, u64)> = bytes
                    .chunks_exact(16)
                    .map(|c| {
                        let fourcc = u32::from_ne_bytes(c[0..4].try_into().unwrap());
                        let modifier = u64::from_ne_bytes(c[8..16].try_into().unwrap());
                        (fourcc, modifier)
                    })
                    .collect();
                log::info!(
                    "dmabuf_feedback: format_table loaded {} entries",
                    entries.len()
                );
                state.dmabuf_format_table = entries;
            }
            zwp_linux_dmabuf_feedback_v1::Event::TrancheFormats { indices } => {
                log::debug!(
                    "dmabuf_feedback: tranche_formats {} indices",
                    indices.len() / 2
                );
            }
            zwp_linux_dmabuf_feedback_v1::Event::TrancheTargetDevice { .. }
            | zwp_linux_dmabuf_feedback_v1::Event::TrancheFlags { .. }
            | zwp_linux_dmabuf_feedback_v1::Event::TrancheDone => {}
            zwp_linux_dmabuf_feedback_v1::Event::Done => {
                log::info!("dmabuf_feedback: done");
            }
            _ => {}
        }
    }
}

impl Dispatch<WpFractionalScaleManagerV1, ()> for App {
    fn event(
        _state: &mut Self,
        _p: &WpFractionalScaleManagerV1,
        _e: wp_fractional_scale_manager_v1::Event,
        _data: &(),
        _conn: &Connection,
        _qh: &QueueHandle<Self>,
    ) {
    }
}

impl Dispatch<WpFractionalScaleV1, u32> for App {
    fn event(
        state: &mut Self,
        _p: &WpFractionalScaleV1,
        event: wp_fractional_scale_v1::Event,
        data: &u32,
        _conn: &Connection,
        _qh: &QueueHandle<Self>,
    ) {
        if let wp_fractional_scale_v1::Event::PreferredScale { scale } = event {
            let output_name = *data;
            let Some(entry) = state.outputs.get_mut(&output_name) else {
                return;
            };
            entry.fractional_scale_120 = scale;
            let Some(binding) = entry.binding.as_ref() else {
                log::info!(
                    "output {output_name}: preferred_scale={scale}/120 (cached, pre-configure)"
                );
                return;
            };
            binding.fractional_scale_120.store(scale, Ordering::SeqCst);
            let logical = *binding.logical_size.lock().unwrap();
            let Some((lw, lh)) = logical else {
                return;
            };
            let physical = if entry.viewport.is_some() {
                let f = scale as u64;
                (
                    ((lw as u64 * f + 60) / 120) as u32,
                    ((lh as u64 * f + 60) / 120) as u32,
                )
            } else {
                let s = entry.scale.max(1) as u32;
                (lw.saturating_mul(s), lh.saturating_mul(s))
            };
            let prev = *binding.configured_size.lock().unwrap();
            if prev == Some(physical) {
                return;
            }
            *binding.configured_size.lock().unwrap() = Some(physical);
            log::info!(
                "output {output_name}: preferred_scale={scale}/120 → physical {}x{}",
                physical.0,
                physical.1
            );
            let arc_binding = binding.clone();
            if let Err(e) = push_resize_if_registered(&arc_binding, physical) {
                log::warn!("output {output_name}: push update_display failed: {e}");
            }
        }
    }
}

impl Dispatch<ZwlrLayerShellV1, ()> for App {
    fn event(
        _state: &mut Self,
        _p: &ZwlrLayerShellV1,
        _e: zwlr_layer_shell_v1::Event,
        _data: &(),
        _conn: &Connection,
        _qh: &QueueHandle<Self>,
    ) {
    }
}

impl Dispatch<ZwlrLayerSurfaceV1, u32> for App {
    fn event(
        state: &mut Self,
        layer_surface: &ZwlrLayerSurfaceV1,
        event: zwlr_layer_surface_v1::Event,
        data: &u32,
        conn: &Connection,
        qh: &QueueHandle<Self>,
    ) {
        let output_name = *data;
        match event {
            zwlr_layer_surface_v1::Event::Configure {
                serial,
                width,
                height,
            } => {
                layer_surface.ack_configure(serial);
                log::info!("output {output_name}: layer_surface configure {width}x{height}");
                let Some(entry) = state.outputs.get_mut(&output_name) else {
                    log::warn!("configure for unknown output_name={output_name}");
                    return;
                };
                if entry.binding.is_none() {
                    let surface = entry
                        .surface
                        .clone()
                        .expect("configure before surface created");
                    let dmabuf = state.dmabuf.clone().expect("configure before dmabuf bind");
                    let display_name = match entry.output_name_str.as_deref() {
                        Some(n) if !n.is_empty() => n.to_string(),
                        _ => format!("{}-{}", state.name_prefix, output_name),
                    };
                    let instance_id = layer_instance_id(entry, output_name);
                    let refresh_mhz = entry.refresh_mhz;
                    log::info!(
                        "output {output_name}: identity '{}' -> instance_id={instance_id}",
                        output_identity_key(entry, output_name)
                    );
                    entry.binding = Some(Arc::new(OutputBinding {
                        display_name,
                        instance_id,
                        surface,
                        dmabuf,
                        conn: conn.clone(),
                        qh: qh.clone(),
                        output_name,
                        configured_size: Mutex::new(None),
                        logical_size: Mutex::new(None),
                        scale: std::sync::atomic::AtomicI32::new(entry.scale.max(1)),
                        fractional_scale_120: AtomicU32::new(entry.fractional_scale_120),
                        refresh_mhz: AtomicU32::new(refresh_mhz),
                        viewport: entry.viewport.clone(),
                        closed: AtomicBool::new(false),
                        display: Mutex::new(None),
                        frame_pending: AtomicBool::new(false),
                        registered: AtomicBool::new(false),
                        last_pushed_size: Mutex::new(None),
                        layer_buffer: Mutex::new(None),
                        config: Mutex::new(FrameConfig::default()),
                        window_flags: AtomicU32::new(0),
                    }));
                }
                let binding = entry.binding.as_ref().expect("binding just created");
                {
                    let mut reg = state.binding_registry.lock().unwrap();
                    reg.insert(binding.display_name().to_string(), binding.clone());
                }
                let scale = entry.scale.max(1);
                binding.scale.store(scale, Ordering::SeqCst);
                let f120 = entry.fractional_scale_120;
                binding.fractional_scale_120.store(f120, Ordering::SeqCst);
                let physical = if f120 > 0 && entry.viewport.is_some() {
                    let f = f120 as u64;
                    (
                        ((width as u64 * f + 60) / 120) as u32,
                        ((height as u64 * f + 60) / 120) as u32,
                    )
                } else {
                    (
                        width.saturating_mul(scale as u32),
                        height.saturating_mul(scale as u32),
                    )
                };
                *binding.logical_size.lock().unwrap() = Some((width, height));
                *binding.configured_size.lock().unwrap() = Some(physical);
                if physical != (width, height) {
                    log::info!(
                        "output {output_name}: logical {width}x{height} → physical {}x{} \
                         (fractional_scale_120={f120}, integer_scale={scale})",
                        physical.0,
                        physical.1
                    );
                }
                let arc_binding = binding.clone();
                if let Err(e) = push_resize_if_registered(&arc_binding, physical) {
                    log::warn!("output {output_name}: push update_display failed: {e}");
                }
                state.maybe_spawn_worker(output_name);
            }
            zwlr_layer_surface_v1::Event::Closed => {
                log::warn!("output {output_name}: layer_surface closed by compositor");
                if let Some(entry) = state.outputs.get_mut(&output_name) {
                    entry.surface = None;
                    entry.layer_surface = None;
                    entry.binding = None;
                    entry.worker_started = false;
                    entry.fractional_scale = None;
                    entry.fractional_scale_120 = 0;
                }
            }
            _ => {}
        }
    }
}

impl Dispatch<ZwpLinuxDmabufV1, ()> for App {
    fn event(
        _state: &mut Self,
        _p: &ZwpLinuxDmabufV1,
        e: zwp_linux_dmabuf_v1::Event,
        _data: &(),
        _conn: &Connection,
        _qh: &QueueHandle<Self>,
    ) {
        match e {
            zwp_linux_dmabuf_v1::Event::Format { .. }
            | zwp_linux_dmabuf_v1::Event::Modifier { .. } => {}
            _ => {}
        }
    }
}

impl Dispatch<ZwpLinuxBufferParamsV1, ()> for App {
    fn event(
        _state: &mut Self,
        _p: &ZwpLinuxBufferParamsV1,
        event: zwp_linux_buffer_params_v1::Event,
        _data: &(),
        _conn: &Connection,
        _qh: &QueueHandle<Self>,
    ) {
        if let zwp_linux_buffer_params_v1::Event::Failed = event {
            log::error!("zwp_linux_buffer_params_v1 Failed: dmabuf import rejected");
        }
    }
}

impl Dispatch<WpViewporter, ()> for App {
    fn event(
        _state: &mut Self,
        _p: &WpViewporter,
        _e: wp_viewporter::Event,
        _data: &(),
        _conn: &Connection,
        _qh: &QueueHandle<Self>,
    ) {
    }
}

impl Dispatch<WpViewport, u32> for App {
    fn event(
        _state: &mut Self,
        _p: &WpViewport,
        _e: wp_viewport::Event,
        _data: &u32,
        _conn: &Connection,
        _qh: &QueueHandle<Self>,
    ) {
    }
}

fn uds_worker_loop(sock: PathBuf, binding: Arc<OutputBinding>) {
    const INITIAL: Duration = Duration::from_secs(2);
    const MAX: Duration = Duration::from_secs(30);
    const STABLE_RESET: Duration = Duration::from_secs(20);
    let mut delay = INITIAL;
    loop {
        if binding.closed.load(Ordering::SeqCst) {
            log::info!("[{}] output closed; worker exiting", binding.display_name);
            return;
        }
        let started = std::time::Instant::now();
        let res = run_uds_session(&sock, &binding);
        let lived = started.elapsed();
        binding.registered.store(false, Ordering::SeqCst);
        binding.last_pushed_size.lock().unwrap().take();
        binding.layer_buffer.lock().unwrap().take();
        binding.display.lock().unwrap().take();
        match res {
            Ok(()) => log::info!("[{}] display session ended cleanly", binding.display_name),
            Err(e) => log::warn!("[{}] display session error: {e:#}", binding.display_name),
        }
        if binding.closed.load(Ordering::SeqCst) {
            log::info!(
                "[{}] output closed; worker exiting after session end",
                binding.display_name
            );
            return;
        }
        if lived >= STABLE_RESET {
            delay = INITIAL;
        }
        log::debug!(
            "[{}] session lived {:?}; reconnecting in {:?}",
            binding.display_name,
            lived,
            delay
        );
        thread::sleep(delay);
        delay = std::cmp::min(delay * 2, MAX);
    }
}

fn push_resize_if_registered(binding: &Arc<OutputBinding>, physical: (u32, u32)) -> Result<()> {
    if !binding.registered.load(Ordering::SeqCst) {
        return Ok(());
    }
    {
        let last = binding.last_pushed_size.lock().unwrap();
        if *last == Some(physical) {
            return Ok(());
        }
    }
    let rc = binding
        .with_display(|d| unsafe { sys::waywallen_display_update_size(d, physical.0, physical.1) });
    if let Some(rc) = rc {
        if rc < 0 {
            return Err(anyhow!("waywallen_display_update_size: {rc}"));
        }
    } else {
        return Ok(());
    }
    *binding.last_pushed_size.lock().unwrap() = Some(physical);
    log::info!(
        "[{}] pushed update_display {}x{}",
        binding.display_name,
        physical.0,
        physical.1
    );
    Ok(())
}

fn run_uds_session(sock: &Path, binding: &Arc<OutputBinding>) -> Result<()> {
    let (width, height) = binding
        .configured_size
        .lock()
        .unwrap()
        .expect("worker started before configure");
    let display_name = CString::new(binding.display_name.as_str()).context("display name")?;
    let instance_id = CString::new(binding.instance_id.as_str()).context("instance id")?;
    let socket_path = CString::new(sock.as_os_str().as_encoded_bytes()).context("socket path")?;

    let callbacks = sys::waywallen_display_callbacks_t {
        on_textures_ready: Some(on_textures_ready),
        on_textures_releasing: Some(on_textures_releasing),
        on_config: Some(on_config),
        on_frame_ready: Some(on_frame_ready),
        on_disconnected: Some(on_disconnected),
        user_data: Arc::as_ptr(binding) as *mut c_void,
    };

    let display = unsafe { sys::waywallen_display_new(&callbacks) };
    if display.is_null() {
        bail!("waywallen_display_new failed");
    }
    {
        *binding.display.lock().unwrap() = Some(DisplayPtr(display));
    }

    let run = (|| -> Result<()> {
        let rc = unsafe { sys::waywallen_display_bind_dmabuf_relay(display) };
        if rc < 0 {
            bail!("waywallen_display_bind_dmabuf_relay failed: {rc}");
        }
        let rc = unsafe {
            sys::waywallen_display_connect(
                display,
                socket_path.as_ptr(),
                display_name.as_ptr(),
                instance_id.as_ptr(),
                width,
                height,
                binding.refresh_mhz.load(Ordering::SeqCst),
            )
        };
        if rc < 0 {
            bail!("waywallen_display_connect failed: {rc}");
        }
        *binding.last_pushed_size.lock().unwrap() = Some((width, height));
        binding.registered.store(true, Ordering::SeqCst);

        let display_id = unsafe { sys::waywallen_display_get_display_id(display) };
        log::info!(
            "[{}] registered as display_id={display_id} instance_id={} ({width}x{height}@{}mHz)",
            binding.display_name,
            binding.instance_id,
            binding.refresh_mhz.load(Ordering::SeqCst)
        );

        if let Some(latest) = *binding.configured_size.lock().unwrap() {
            if latest != (width, height) {
                push_resize_if_registered(binding, latest)?;
            }
        }
        let flags = binding.window_flags.load(Ordering::SeqCst);
        if flags != 0 {
            unsafe {
                sys::waywallen_display_set_window_state(display, flags);
            }
        }

        dispatch_display_loop(display, binding)
    })();

    unsafe {
        sys::waywallen_display_shutdown(display);
    }
    run
}

fn dispatch_display_loop(
    display: *mut sys::waywallen_display_t,
    binding: &OutputBinding,
) -> Result<()> {
    loop {
        if binding.closed.load(Ordering::SeqCst) {
            return Ok(());
        }
        let fd = unsafe { sys::waywallen_display_get_fd(display) };
        if fd < 0 {
            return Ok(());
        }
        let mut pfd = libc::pollfd {
            fd,
            events: libc::POLLIN,
            revents: 0,
        };
        if unsafe { sys::waywallen_display_wants_writable(display) } {
            pfd.events |= libc::POLLOUT;
        }
        let rc = unsafe { libc::poll(&mut pfd, 1, 500) };
        if rc < 0 {
            let err = std::io::Error::last_os_error();
            if err.kind() == std::io::ErrorKind::Interrupted {
                continue;
            }
            return Err(err).context("poll display fd");
        }
        if rc == 0 {
            continue;
        }
        if pfd.revents & libc::POLLOUT != 0 {
            let r = unsafe { sys::waywallen_display_handle_writable(display) };
            if r < 0 {
                bail!("waywallen_display_handle_writable failed: {r}");
            }
        }
        if pfd.revents & (libc::POLLIN | libc::POLLERR | libc::POLLHUP) != 0 {
            let r = unsafe { sys::waywallen_display_dispatch(display) };
            if r < 0 {
                bail!("waywallen_display_dispatch failed: {r}");
            }
        }
    }
}

unsafe extern "C" fn on_textures_ready(
    user_data: *mut c_void,
    t: *const sys::waywallen_textures_t,
) {
    let binding = binding_from_user_data(user_data);
    if t.is_null() {
        return;
    }
    let t = &*t;
    if t.backend != sys::WAYWALLEN_BACKEND_DMABUF_RELAY || t.shadow_dmabuf_fd < 0 {
        log::warn!(
            "[{}] textures_ready without dmabuf relay shadow fd",
            binding.display_name
        );
        return;
    }
    match import_shadow_dmabuf(binding, t) {
        Ok(buffer) => {
            *binding.layer_buffer.lock().unwrap() = Some(buffer);
            log::info!(
                "[{}] imported relay shadow wl_buffer {}x{} fourcc=0x{:08x}",
                binding.display_name,
                t.tex_width,
                t.tex_height,
                t.fourcc
            );
        }
        Err(e) => log::warn!(
            "[{}] import relay shadow failed: {e:#}",
            binding.display_name
        ),
    }
}

unsafe extern "C" fn on_textures_releasing(
    user_data: *mut c_void,
    _t: *const sys::waywallen_textures_t,
) {
    let binding = binding_from_user_data(user_data);
    binding.layer_buffer.lock().unwrap().take();
}

unsafe extern "C" fn on_config(user_data: *mut c_void, c: *const sys::waywallen_config_t) {
    let binding = binding_from_user_data(user_data);
    if c.is_null() {
        return;
    }
    let c = &*c;
    let mut cfg = binding.config.lock().unwrap();
    cfg.source = Some((
        c.source_rect.x,
        c.source_rect.y,
        c.source_rect.w,
        c.source_rect.h,
    ));
    if cfg.transform != c.transform {
        cfg.transform = c.transform;
        cfg.transform_dirty = true;
    }
}

unsafe extern "C" fn on_frame_ready(user_data: *mut c_void, f: *const sys::waywallen_frame_t) {
    let binding = binding_from_user_data(user_data);
    if f.is_null() {
        return;
    }
    let f = &*f;
    if f.release_syncobj_fd >= 0 {
        let _ = sys::waywallen_display_signal_release_syncobj(f.release_syncobj_fd);
    }
    if let Err(e) = present_shadow(binding) {
        log::warn!(
            "[{}] present frame seq={} failed: {e:#}",
            binding.display_name,
            f.seq
        );
    }
}

unsafe extern "C" fn on_disconnected(user_data: *mut c_void, err: i32, msg: *const i8) {
    let binding = binding_from_user_data(user_data);
    let msg = if msg.is_null() {
        ""
    } else {
        CStr::from_ptr(msg).to_str().unwrap_or("")
    };
    log::warn!("[{}] disconnected: {err}: {msg}", binding.display_name);
}

unsafe fn binding_from_user_data<'a>(user_data: *mut c_void) -> &'a OutputBinding {
    &*(user_data as *const OutputBinding)
}

fn import_shadow_dmabuf(
    binding: &OutputBinding,
    t: &sys::waywallen_textures_t,
) -> Result<LayerBuffer> {
    let fd = unsafe { libc::dup(t.shadow_dmabuf_fd) };
    if fd < 0 {
        return Err(std::io::Error::last_os_error()).context("dup shadow dmabuf fd");
    }
    let fd = unsafe { OwnedFd::from_raw_fd(fd) };
    let params = binding.dmabuf.create_params(&binding.qh, ());
    let modifier = t.shadow_modifier;
    let mod_hi = (modifier >> 32) as u32;
    let mod_lo = (modifier & 0xffff_ffff) as u32;
    let n_planes = t.shadow_n_planes.min(4);
    for plane in 0..n_planes {
        params.add(
            fd.as_fd(),
            plane,
            t.shadow_offsets[plane as usize] as u32,
            t.shadow_strides[plane as usize],
            mod_hi,
            mod_lo,
        );
    }
    let wl_buffer = params.create_immed(
        t.tex_width as i32,
        t.tex_height as i32,
        t.fourcc,
        zwp_linux_buffer_params_v1::Flags::empty(),
        &binding.qh,
        (binding.output_name, 0),
    );
    Ok(LayerBuffer {
        wl_buffer,
        width: t.tex_width,
        height: t.tex_height,
    })
}

fn present_shadow(binding: &OutputBinding) -> Result<()> {
    let mut cfg = binding.config.lock().unwrap();
    let guard = binding.layer_buffer.lock().unwrap();
    let Some(buffer) = guard.as_ref() else {
        return Ok(());
    };

    binding.surface.attach(Some(&buffer.wl_buffer), 0, 0);
    let src = cfg
        .source
        .unwrap_or((0.0, 0.0, buffer.width as f32, buffer.height as f32));
    let logical = binding
        .logical_size
        .lock()
        .unwrap()
        .unwrap_or((buffer.width, buffer.height));

    if let Some(vp) = binding.viewport.as_ref() {
        vp.set_source(src.0 as f64, src.1 as f64, src.2 as f64, src.3 as f64);
        vp.set_destination(logical.0 as i32, logical.1 as i32);
    } else {
        let scale = binding.scale.load(Ordering::SeqCst);
        if scale > 1 {
            binding.surface.set_buffer_scale(scale);
        }
    }

    if cfg.transform_dirty {
        binding
            .surface
            .set_buffer_transform(wl_transform(cfg.transform));
        cfg.transform_dirty = false;
    }

    binding
        .surface
        .damage_buffer(0, 0, buffer.width as i32, buffer.height as i32);
    binding.surface.frame(&binding.qh, binding.output_name);
    binding.frame_pending.store(true, Ordering::SeqCst);
    binding.surface.commit();
    binding.conn.flush().context("wayland flush")?;
    Ok(())
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

/// Run the layer-shell display backend. Connects to the Wayland compositor
/// and the daemon's UDS display socket, registers each output as a display.
/// Blocks until the compositor disconnects or an unrecoverable error occurs.
fn usage() -> ! {
    eprintln!(
        "usage: waywallen-layer-shell [--socket PATH] [--name STR]\n\
         \n\
         Environment:\n\
           WAYWALLEN_SOCKET   fallback UDS path when --socket is omitted\n\
           WAYLAND_DISPLAY    required — picks the compositor to attach to"
    );
    std::process::exit(2);
}

fn main() -> Result<()> {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info")).init();

    let mut socket: Option<PathBuf> = None;
    let mut name_prefix = String::from("output");
    let mut it = std::env::args().skip(1);
    while let Some(arg) = it.next() {
        match arg.as_str() {
            "--socket" => {
                socket = it.next().map(PathBuf::from);
                if socket.is_none() {
                    eprintln!("--socket requires a value");
                    usage();
                }
            }
            "--name" => {
                name_prefix = it.next().unwrap_or_else(|| {
                    eprintln!("--name requires a value");
                    usage();
                });
            }
            "-h" | "--help" => usage(),
            other => {
                eprintln!("unknown argument: {other}");
                usage();
            }
        }
    }
    let socket = socket
        .or_else(|| std::env::var_os("WAYWALLEN_SOCKET").map(PathBuf::from))
        .unwrap_or_else(default_socket_path);

    run(socket, name_prefix)
}

fn run(socket: PathBuf, name_prefix: String) -> Result<()> {
    let conn = Connection::connect_to_env()
        .context("connect to WAYLAND_DISPLAY — are you running under a Wayland compositor?")?;
    let (globals, mut queue) = registry_queue_init::<App>(&conn).context("registry init")?;
    let qh: QueueHandle<App> = queue.handle();

    let mut app = App::new(socket, name_prefix);

    watcher::hyprland::spawn(app.binding_registry.clone());
    watcher::niri::spawn(app.binding_registry.clone());
    watcher::wayfire::spawn(app.binding_registry.clone());

    for g in globals.contents().clone_list() {
        match g.interface.as_str() {
            "wl_compositor" => {
                app.compositor = Some(globals.registry().bind::<WlCompositor, _, _>(
                    g.name,
                    g.version.min(6),
                    &qh,
                    (),
                ));
            }
            "zwlr_layer_shell_v1" => {
                app.layer_shell = Some(globals.registry().bind::<ZwlrLayerShellV1, _, _>(
                    g.name,
                    g.version.min(4),
                    &qh,
                    (),
                ));
            }
            "zwp_linux_dmabuf_v1" => {
                let dmabuf = globals.registry().bind::<ZwpLinuxDmabufV1, _, _>(
                    g.name,
                    g.version.min(4),
                    &qh,
                    (),
                );
                if dmabuf.version() >= 4 {
                    app.dmabuf_feedback = Some(dmabuf.get_default_feedback(&qh, ()));
                }
                app.dmabuf = Some(dmabuf);
            }
            "wp_viewporter" => {
                app.viewporter = Some(globals.registry().bind::<WpViewporter, _, _>(
                    g.name,
                    g.version.min(1),
                    &qh,
                    (),
                ));
            }
            "wp_fractional_scale_manager_v1" => {
                app.fractional_scale_mgr =
                    Some(globals.registry().bind::<WpFractionalScaleManagerV1, _, _>(
                        g.name,
                        g.version.min(1),
                        &qh,
                        (),
                    ));
            }
            "wl_output" => {
                let wl_output = globals.registry().bind::<WlOutput, _, _>(
                    g.name,
                    g.version.min(4),
                    &qh,
                    g.name,
                );
                app.outputs.insert(
                    g.name,
                    OutputEntry {
                        wl_output,
                        surface: None,
                        layer_surface: None,
                        viewport: None,
                        binding: None,
                        worker_started: false,
                        scale: 1,
                        fractional_scale: None,
                        fractional_scale_120: 0,
                        refresh_mhz: 60_000,
                        output_name_str: None,
                        output_description: None,
                        output_make: None,
                        output_model: None,
                    },
                );
            }
            "wl_seat" => {
                globals
                    .registry()
                    .bind::<WlSeat, _, _>(g.name, g.version.min(5), &qh, g.name);
            }
            _ => {}
        }
    }

    if app.compositor.is_none() {
        bail!("compositor does not expose wl_compositor");
    }
    if app.layer_shell.is_none() {
        bail!(
            "compositor does not expose zwlr_layer_shell_v1 — \
             try a different compositor (Hyprland/Sway/KWin/new Mutter)"
        );
    }
    if app.dmabuf.is_none() {
        bail!("compositor does not expose zwp_linux_dmabuf_v1");
    }
    if app.outputs.is_empty() {
        bail!("no wl_output available");
    }
    log::info!(
        "bound globals: compositor + layer_shell + dmabuf:v{} + viewporter:{} + \
         fractional_scale:{} + dmabuf_feedback:{} + {} output(s)",
        app.dmabuf.as_ref().map(|d| d.version()).unwrap_or(0),
        app.viewporter.is_some(),
        app.fractional_scale_mgr.is_some(),
        app.dmabuf_feedback.is_some(),
        app.outputs.len()
    );

    queue
        .roundtrip(&mut app)
        .context("initial wl_output metadata roundtrip")?;

    let output_keys: Vec<u32> = app.outputs.keys().copied().collect();
    for name in output_keys {
        app.bring_up_surface(name, &qh);
    }

    loop {
        if let Err(e) = queue.blocking_dispatch(&mut app) {
            log::error!("wayland dispatch error: {e}");
            return Err(e.into());
        }
    }
}
