//! waywallen-layer-shell — Wayland layer-shell wallpaper client.
//!
//! Connects to a Wayland compositor that supports `zwlr_layer_shell_v1`
//! (Hyprland, Sway, Niri, River, …) and registers each output as a
//! display with the daemon over the waywallen-display UDS protocol.

mod vulkan;
mod watcher;

use std::collections::HashMap;
use std::ffi::{c_char, c_void, CStr, CString};
use std::os::fd::AsRawFd;
use std::os::unix::fs::FileExt;
use std::path::{Path, PathBuf};
use std::rc::Rc;
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use anyhow::{anyhow, bail, Context, Result};
use ash::vk::{self, Handle};
use md5::{Digest, Md5};
use wayland_client::globals::{registry_queue_init, GlobalListContents};
use wayland_client::protocol::{
    wl_compositor::WlCompositor,
    wl_output::{self, WlOutput},
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
    configured_size: Mutex<Option<(u32, u32)>>,
    scale: std::sync::atomic::AtomicI32,
    fractional_scale_120: AtomicU32,
    refresh_mhz: AtomicU32,
    display: Mutex<Option<DisplayPtr>>,
    registered: AtomicBool,
    last_pushed_metrics: Mutex<Option<(u32, u32, u32)>>,
    config: Mutex<FrameConfig>,
    runtime: Arc<vulkan::VulkanRuntime>,
    presenter: Mutex<vulkan::WsiPresenter>,
    pending_present: AtomicBool,
    next_redraw: Mutex<Option<Instant>>,
    watcher: Arc<watcher::OutputInfo>,
}

impl OutputBinding {
    pub fn display_name(&self) -> &str {
        &self.display_name
    }
    fn with_display<F>(&self, f: F) -> Option<i32>
    where
        F: FnOnce(*mut sys::waywallen_display_t) -> i32,
    {
        let guard = self.display.lock().unwrap();
        guard.as_ref().map(|d| f(d.0))
    }
}

#[derive(Copy, Clone)]
struct DisplayPtr(*mut sys::waywallen_display_t);

const INITIAL_RECONNECT_DELAY: Duration = Duration::from_secs(2);
const MAX_RECONNECT_DELAY: Duration = Duration::from_secs(30);
const STABLE_SESSION_TIME: Duration = Duration::from_secs(20);

enum DisplaySessionState {
    Handshake { events: i16 },
    Ready,
    Retiring,
}

struct DisplaySession {
    display: DisplayPtr,
    state: DisplaySessionState,
    started: Instant,
    _binding: Rc<OutputBinding>,
}

impl DisplaySession {
    fn is_ready(&self) -> bool {
        matches!(self.state, DisplaySessionState::Ready)
    }

    fn poll_events(&self) -> i16 {
        match self.state {
            DisplaySessionState::Handshake { events } => events,
            DisplaySessionState::Ready => {
                let mut events = libc::POLLIN;
                if unsafe { sys::waywallen_display_wants_writable(self.display.0) } {
                    events |= libc::POLLOUT;
                }
                events
            }
            DisplaySessionState::Retiring => 0,
        }
    }
}

#[derive(Clone, Copy)]
struct FrameConfig {
    source: [f32; 4],
    destination: [f32; 4],
    transform: u32,
    clear: [f32; 4],
}

impl Default for FrameConfig {
    fn default() -> Self {
        Self {
            source: [0.0; 4],
            destination: [0.0; 4],
            transform: 0,
            clear: [0.0; 4],
        }
    }
}

struct OutputEntry {
    wl_output: WlOutput,
    surface: Option<WlSurface>,
    layer_surface: Option<ZwlrLayerSurfaceV1>,
    viewport: Option<WpViewport>,
    binding: Option<Rc<OutputBinding>>,
    session: Option<DisplaySession>,
    reconnect_at: Instant,
    reconnect_delay: Duration,
    scale: i32,
    fractional_scale: Option<WpFractionalScaleV1>,
    fractional_scale_120: u32,
    vk_surface: Option<ash::vk::SurfaceKHR>,
    configured_size: Option<(u32, u32)>,
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
    watcher_commands: watcher::CommandReceiver,
    vulkan: Option<Arc<vulkan::VulkanRuntime>>,
}

struct PointerCtx {
    pointer: WlPointer,
    focus_output: Option<u32>,
    last_x: f64,
    last_y: f64,
    axis_source: u32,
}

impl App {
    fn new(
        uds_sock: PathBuf,
        name_prefix: String,
        watcher_commands: watcher::CommandReceiver,
    ) -> Self {
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
            watcher_commands,
            vulkan: None,
        }
    }

    fn bring_up_surface(
        &mut self,
        output_name: u32,
        conn: &Connection,
        qh: &QueueHandle<App>,
    ) -> bool {
        let Some(entry) = self.outputs.get_mut(&output_name) else {
            return false;
        };
        if entry.surface.is_some() {
            return false;
        }
        let (Some(comp), Some(shell)) = (self.compositor.as_ref(), self.layer_shell.as_ref())
        else {
            return false;
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
        let mut rebuild_runtime = false;
        if let Some(runtime) = self.vulkan.as_ref() {
            match runtime.create_surface(conn, &surface) {
                Ok(vk_surface) => entry.vk_surface = Some(vk_surface),
                Err(error) => {
                    rebuild_runtime = true;
                    log::warn!(
                        "output {output_name}: current Vulkan device cannot own the new surface: \
                         {error:#}"
                    );
                }
            }
        }
        entry.surface = Some(surface);
        entry.layer_surface = Some(layer_surface);
        entry.viewport = viewport;
        entry.fractional_scale = fractional_scale;
        log::info!("output {output_name}: layer_surface committed, waiting for configure");
        rebuild_runtime
    }

    fn rebuild_vulkan_runtime(&mut self, conn: &Connection) -> Result<()> {
        let wayland_surfaces = self
            .outputs
            .iter()
            .filter_map(|(name, entry)| entry.surface.clone().map(|surface| (*name, surface)))
            .collect::<Vec<_>>();
        let (runtime, surfaces) = vulkan::VulkanRuntime::new(
            conn,
            &wayland_surfaces,
            (self.compositor_drm_major, self.compositor_drm_minor),
        )?;
        let mut raw_surfaces = surfaces.into_iter().collect::<HashMap<_, _>>();
        let mut presenters = HashMap::new();
        for (output_name, entry) in &self.outputs {
            let Some(physical) = entry.configured_size else {
                continue;
            };
            let Some(surface) = raw_surfaces.remove(output_name) else {
                for surface in raw_surfaces.into_values() {
                    runtime.destroy_surface(surface);
                }
                return Err(anyhow!("candidate runtime omitted output {output_name}"));
            };
            match vulkan::WsiPresenter::new(Arc::clone(&runtime), surface, physical) {
                Ok(presenter) => {
                    presenters.insert(*output_name, presenter);
                }
                Err(error) => {
                    for surface in raw_surfaces.into_values() {
                        runtime.destroy_surface(surface);
                    }
                    return Err(error).with_context(|| {
                        format!("create candidate presenter for output {output_name}")
                    });
                }
            }
        }

        let old_runtime = self
            .vulkan
            .take()
            .ok_or_else(|| anyhow!("runtime rebuild requested before Vulkan initialization"))?;
        let mut watchers = HashMap::new();
        self.binding_registry.lock().unwrap().clear();
        for (output_name, entry) in &mut self.outputs {
            if let Some(binding) = entry.binding.as_ref() {
                watchers.insert(*output_name, Arc::clone(&binding.watcher));
            }
            if let Some(session) = entry.session.take() {
                if let Some(binding) = entry.binding.as_ref() {
                    shutdown_display_session(binding, session);
                }
            }
            entry.binding.take();
            if let Some(surface) = entry.vk_surface.take() {
                old_runtime.destroy_surface(surface);
            }
        }
        drop(old_runtime);

        let name_prefix = self.name_prefix.clone();
        for (output_name, entry) in &mut self.outputs {
            if let Some(presenter) = presenters.remove(output_name) {
                let physical = entry
                    .configured_size
                    .expect("candidate presenter requires configured size");
                let binding = make_output_binding(
                    &name_prefix,
                    entry,
                    *output_name,
                    Arc::clone(&runtime),
                    presenter,
                    physical,
                    watchers.remove(output_name),
                );
                self.binding_registry
                    .lock()
                    .unwrap()
                    .insert(binding.display_name.clone(), Arc::clone(&binding.watcher));
                entry.binding = Some(binding);
                entry.reconnect_at = Instant::now();
                entry.reconnect_delay = INITIAL_RECONNECT_DELAY;
            } else if let Some(surface) = raw_surfaces.remove(output_name) {
                entry.vk_surface = Some(surface);
            }
        }
        for surface in raw_surfaces.into_values() {
            runtime.destroy_surface(surface);
        }
        self.vulkan = Some(runtime);
        log::info!(
            "rebuilt shared Vulkan runtime for {} active Wayland output(s)",
            wayland_surfaces.len()
        );
        Ok(())
    }

    fn start_due_sessions(&mut self) {
        let now = Instant::now();
        let due: Vec<u32> = self
            .outputs
            .iter()
            .filter_map(|(name, entry)| {
                (entry.session.is_none() && entry.binding.is_some() && entry.reconnect_at <= now)
                    .then_some(*name)
            })
            .collect();
        for output_name in due {
            let Some(binding) = self
                .outputs
                .get(&output_name)
                .and_then(|entry| entry.binding.as_ref())
                .cloned()
            else {
                continue;
            };
            match start_display_session(&self.uds_sock, &binding) {
                Ok(session) => {
                    if let Some(entry) = self.outputs.get_mut(&output_name) {
                        entry.session = Some(session);
                        log::debug!(
                            "output {output_name}: display session started for '{}'",
                            binding.display_name
                        );
                    }
                }
                Err(error) => {
                    log::warn!(
                        "[{}] display session start failed: {error:#}",
                        binding.display_name
                    );
                    self.schedule_reconnect(output_name, Duration::ZERO);
                }
            }
        }
    }

    fn schedule_reconnect(&mut self, output_name: u32, lived: Duration) {
        let Some(entry) = self.outputs.get_mut(&output_name) else {
            return;
        };
        if lived >= STABLE_SESSION_TIME {
            entry.reconnect_delay = INITIAL_RECONNECT_DELAY;
        }
        let delay = entry.reconnect_delay;
        entry.reconnect_at = Instant::now() + delay;
        entry.reconnect_delay = std::cmp::min(delay * 2, MAX_RECONNECT_DELAY);
        if let Some(binding) = entry.binding.as_ref() {
            log::debug!(
                "[{}] session lived {:?}; reconnecting in {:?}",
                binding.display_name,
                lived,
                delay
            );
        }
    }

    fn finish_session(&mut self, output_name: u32, error: &anyhow::Error) {
        let Some(entry) = self.outputs.get_mut(&output_name) else {
            return;
        };
        let Some(session) = entry.session.as_mut() else {
            return;
        };
        if let Some(binding) = entry.binding.as_ref() {
            log::warn!(
                "[{}] display session error: {error:#}",
                binding.display_name
            );
            binding.registered.store(false, Ordering::SeqCst);
            binding.pending_present.store(false, Ordering::SeqCst);
            binding.next_redraw.lock().unwrap().take();
            binding.last_pushed_metrics.lock().unwrap().take();
        }
        session.state = DisplaySessionState::Retiring;
    }

    fn retire_finished_sessions(&mut self) {
        let ready: Vec<u32> = self
            .outputs
            .iter()
            .filter_map(|(name, entry)| {
                let session = entry.session.as_ref()?;
                if !matches!(session.state, DisplaySessionState::Retiring) {
                    return None;
                }
                let binding = entry.binding.as_ref()?;
                match binding.presenter.lock().unwrap().frames_idle() {
                    Ok(true) => Some(*name),
                    Ok(false) => None,
                    Err(error) => {
                        log::warn!(
                            "[{}] query retiring frame fences failed: {error:#}",
                            binding.display_name
                        );
                        Some(*name)
                    }
                }
            })
            .collect();
        for output_name in ready {
            let Some(entry) = self.outputs.get_mut(&output_name) else {
                continue;
            };
            let Some(session) = entry.session.take() else {
                continue;
            };
            let lived = session.started.elapsed();
            if let Some(binding) = entry.binding.as_ref() {
                shutdown_display_session(binding, session);
            }
            self.schedule_reconnect(output_name, lived);
        }
    }

    fn display_poll_sources(&self) -> Vec<(u32, libc::pollfd)> {
        self.outputs
            .iter()
            .filter_map(|(name, entry)| {
                let session = entry.session.as_ref()?;
                if matches!(session.state, DisplaySessionState::Retiring) {
                    return None;
                }
                let fd = unsafe { sys::waywallen_display_get_fd(session.display.0) };
                (fd >= 0).then_some((
                    *name,
                    libc::pollfd {
                        fd,
                        events: session.poll_events(),
                        revents: 0,
                    },
                ))
            })
            .collect()
    }

    fn process_display_poll(&mut self, output_name: u32, revents: i16) {
        let result = (|| -> Result<()> {
            let entry = self
                .outputs
                .get_mut(&output_name)
                .ok_or_else(|| anyhow!("poll result for removed output {output_name}"))?;
            let binding = entry
                .binding
                .as_ref()
                .cloned()
                .ok_or_else(|| anyhow!("display session without output binding"))?;
            let session = entry
                .session
                .as_mut()
                .ok_or_else(|| anyhow!("poll result without display session"))?;
            process_display_event(&binding, session, revents)
        })();
        if let Err(error) = result {
            self.finish_session(output_name, &error);
        }
    }

    fn drain_watcher_commands(&mut self) {
        for command in self.watcher_commands.drain() {
            match command {
                watcher::Command::WindowState {
                    display_name,
                    flags,
                } => {
                    let target = self.outputs.values().find_map(|entry| {
                        let binding = entry.binding.as_ref()?;
                        let session = entry.session.as_ref()?;
                        (binding.display_name == display_name
                            && !matches!(session.state, DisplaySessionState::Retiring))
                        .then_some((binding, session))
                    });
                    let Some((binding, session)) = target else {
                        continue;
                    };
                    let rc = unsafe {
                        sys::waywallen_display_set_window_state(session.display.0, flags)
                    };
                    if rc >= 0 {
                        log::debug!(
                            "watcher: [{}] window_state flags=0x{flags:x}",
                            binding.display_name
                        );
                    } else {
                        log::warn!(
                            "watcher: [{}] send window_state failed: {rc}",
                            binding.display_name
                        );
                    }
                }
            }
        }
    }

    fn poll_timeout_ms(&self) -> i32 {
        if self.outputs.values().any(|entry| {
            entry
                .session
                .as_ref()
                .is_some_and(|session| matches!(session.state, DisplaySessionState::Retiring))
                || entry
                    .binding
                    .as_ref()
                    .is_some_and(|binding| binding.pending_present.load(Ordering::SeqCst))
        }) {
            return 8;
        }
        let now = Instant::now();
        let until_redraw = self
            .outputs
            .values()
            .filter_map(|entry| {
                let binding = entry.binding.as_ref()?;
                binding
                    .next_redraw
                    .lock()
                    .unwrap()
                    .map(|deadline| deadline.saturating_duration_since(now))
            })
            .min();
        let until_reconnect = self
            .outputs
            .values()
            .filter(|entry| entry.session.is_none() && entry.binding.is_some())
            .map(|entry| entry.reconnect_at.saturating_duration_since(now))
            .min()
            .unwrap_or(Duration::from_millis(500));
        let timeout = until_redraw
            .map(|redraw| redraw.min(until_reconnect))
            .unwrap_or(until_reconnect)
            .min(Duration::from_millis(500));
        let millis = timeout.as_millis();
        i32::try_from(if timeout.is_zero() { 0 } else { millis.max(1) }).unwrap_or(500)
    }

    fn pump_presenters(&mut self) {
        let now = Instant::now();
        let pending = self
            .outputs
            .iter()
            .filter_map(|(name, entry)| {
                let binding = entry.binding.as_ref()?;
                let due = binding.pending_present.load(Ordering::SeqCst)
                    || binding
                        .next_redraw
                        .lock()
                        .unwrap()
                        .is_some_and(|deadline| deadline <= now);
                due.then_some((*name, Rc::clone(binding)))
            })
            .collect::<Vec<_>>();
        for (output_name, binding) in pending {
            if let Err(error) = present_latest(&binding) {
                binding.pending_present.store(false, Ordering::SeqCst);
                binding.next_redraw.lock().unwrap().take();
                log::warn!(
                    "[{}] present pending frame failed: {error:#}",
                    binding.display_name
                );
                self.finish_session(output_name, &error);
            }
        }
    }
}

impl Drop for App {
    fn drop(&mut self) {
        let outputs = std::mem::take(&mut self.outputs);
        for mut entry in outputs.into_values() {
            if let (Some(binding), Some(session)) = (entry.binding.as_ref(), entry.session.take()) {
                shutdown_display_session(binding, session);
            } else if entry.binding.is_none() {
                if let (Some(runtime), Some(surface)) =
                    (self.vulkan.as_ref(), entry.vk_surface.take())
                {
                    runtime.destroy_surface(surface);
                }
            }
        }
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
                            session: None,
                            reconnect_at: Instant::now(),
                            reconnect_delay: INITIAL_RECONNECT_DELAY,
                            scale: 1,
                            fractional_scale: None,
                            fractional_scale_120: 0,
                            vk_surface: None,
                            configured_size: None,
                            refresh_mhz: 60_000,
                            output_name_str: None,
                            output_description: None,
                            output_make: None,
                            output_model: None,
                        },
                    );
                    log::info!("hot-plug: wl_output name={name} added; bringing up surface");
                    if state.bring_up_surface(name, _conn, qh) {
                        if let Err(error) = state.rebuild_vulkan_runtime(_conn) {
                            log::error!(
                                "hot-plug: no shared Vulkan runtime for output {name}; \
                                 existing outputs preserved: {error:#}"
                            );
                        }
                    }
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
                        state
                            .binding_registry
                            .lock()
                            .unwrap()
                            .remove(binding.display_name());
                    } else if let (Some(runtime), Some(surface)) =
                        (state.vulkan.as_ref(), entry.vk_surface)
                    {
                        runtime.destroy_surface(surface);
                    }
                    shutdown_output_entry(entry);
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

fn make_output_binding(
    name_prefix: &str,
    entry: &OutputEntry,
    output_name: u32,
    runtime: Arc<vulkan::VulkanRuntime>,
    presenter: vulkan::WsiPresenter,
    physical: (u32, u32),
    existing_watcher: Option<Arc<watcher::OutputInfo>>,
) -> Rc<OutputBinding> {
    let display_name = match entry.output_name_str.as_deref() {
        Some(name) if !name.is_empty() => name.to_string(),
        _ => format!("{name_prefix}-{output_name}"),
    };
    let instance_id = layer_instance_id(entry, output_name);
    let watcher = existing_watcher
        .unwrap_or_else(|| Arc::new(watcher::OutputInfo::new(display_name.clone())));
    log::info!(
        "output {output_name}: identity '{}' -> instance_id={instance_id}",
        output_identity_key(entry, output_name)
    );
    Rc::new(OutputBinding {
        display_name,
        instance_id,
        configured_size: Mutex::new(Some(physical)),
        scale: std::sync::atomic::AtomicI32::new(entry.scale.max(1)),
        fractional_scale_120: AtomicU32::new(entry.fractional_scale_120),
        refresh_mhz: AtomicU32::new(entry.refresh_mhz),
        display: Mutex::new(None),
        registered: AtomicBool::new(false),
        last_pushed_metrics: Mutex::new(None),
        config: Mutex::new(FrameConfig::default()),
        runtime,
        presenter: Mutex::new(presenter),
        pending_present: AtomicBool::new(false),
        next_redraw: Mutex::new(None),
        watcher,
    })
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

fn physical_output_size(
    logical: (u32, u32),
    integer_scale: i32,
    fractional_scale_120: u32,
    has_viewport: bool,
) -> (u32, u32) {
    if fractional_scale_120 > 0 && has_viewport {
        let scale = fractional_scale_120 as u64;
        return (
            ((logical.0 as u64 * scale + 60) / 120) as u32,
            ((logical.1 as u64 * scale + 60) / 120) as u32,
        );
    }
    let scale = integer_scale.max(1) as u32;
    (
        logical.0.saturating_mul(scale),
        logical.1.saturating_mul(scale),
    )
}

fn send_pointer_motion(state: &App, output_name: u32, x: f32, y: f32, timestamp_us: u64) {
    let Some(entry) = state.outputs.get(&output_name) else {
        return;
    };
    let Some(binding) = entry.binding.as_ref() else {
        return;
    };
    let Some(session) = entry.session.as_ref().filter(|session| session.is_ready()) else {
        return;
    };
    let rc = unsafe {
        sys::waywallen_display_send_pointer_motion(session.display.0, x, y, timestamp_us, 0)
    };
    if rc < 0 {
        log::debug!(
            "[{}] send pointer_motion failed: {rc}",
            binding.display_name
        );
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
    let Some(session) = state
        .outputs
        .get(&output_name)
        .and_then(|entry| entry.session.as_ref())
        .filter(|session| session.is_ready())
    else {
        return;
    };
    let button_state = if state_u32 == 1 {
        sys::WAYWALLEN_POINTER_BUTTON_STATE_PRESSED
    } else {
        sys::WAYWALLEN_POINTER_BUTTON_STATE_RELEASED
    };
    let rc = unsafe {
        sys::waywallen_display_send_pointer_button(
            session.display.0,
            x,
            y,
            button,
            button_state,
            timestamp_us,
            0,
        )
    };
    if rc < 0 {
        log::debug!(
            "[{}] send pointer_button failed: {rc}",
            binding.display_name
        );
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
    let Some(session) = state
        .outputs
        .get(&output_name)
        .and_then(|entry| entry.session.as_ref())
        .filter(|session| session.is_ready())
    else {
        return;
    };
    let source = match source {
        1 => sys::WAYWALLEN_POINTER_AXIS_SOURCE_FINGER,
        2 => sys::WAYWALLEN_POINTER_AXIS_SOURCE_CONTINUOUS,
        _ => sys::WAYWALLEN_POINTER_AXIS_SOURCE_WHEEL,
    };
    let rc = unsafe {
        sys::waywallen_display_send_pointer_axis(
            session.display.0,
            x,
            y,
            delta_x,
            delta_y,
            source,
            timestamp_us,
            0,
        )
    };
    if rc < 0 {
        log::debug!("[{}] send pointer_axis failed: {rc}", binding.display_name);
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
                        if entry.fractional_scale_120 == 0 {
                            if let Some((width, height)) = binding.watcher.logical_size() {
                                let physical = (
                                    width.saturating_mul(factor.max(1) as u32),
                                    height.saturating_mul(factor.max(1) as u32),
                                );
                                *binding.configured_size.lock().unwrap() = Some(physical);
                                if let Some(surface) = entry.surface.as_ref() {
                                    surface.set_buffer_scale(factor.max(1));
                                }
                                binding
                                    .presenter
                                    .lock()
                                    .unwrap()
                                    .request_resize(physical.0, physical.1);
                                if let Err(error) = push_resize_if_registered(binding, physical) {
                                    log::warn!(
                                        "output {output_name}: push display metrics failed: {error}"
                                    );
                                }
                            }
                        }
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
                            if let Some(physical) = *binding.configured_size.lock().unwrap() {
                                if let Err(e) = push_resize_if_registered(binding, physical) {
                                    log::warn!(
                                        "output {output_name}: push display metrics failed: {e}"
                                    );
                                }
                            }
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
            let logical = binding.watcher.logical_size();
            let Some((lw, lh)) = logical else {
                return;
            };
            let physical =
                physical_output_size((lw, lh), entry.scale, scale, entry.viewport.is_some());
            let prev = *binding.configured_size.lock().unwrap();
            if prev == Some(physical) {
                return;
            }
            entry.configured_size = Some(physical);
            *binding.configured_size.lock().unwrap() = Some(physical);
            if let Some(surface) = entry.surface.as_ref() {
                surface.set_buffer_scale(1);
            }
            if let Some(viewport) = entry.viewport.as_ref() {
                viewport.set_destination(lw as i32, lh as i32);
            }
            binding
                .presenter
                .lock()
                .unwrap()
                .request_resize(physical.0, physical.1);
            log::info!(
                "output {output_name}: preferred_scale={scale}/120 → physical {}x{}",
                physical.0,
                physical.1
            );
            let arc_binding = binding.clone();
            if let Err(e) = push_resize_if_registered(&arc_binding, physical) {
                log::warn!("output {output_name}: push display metrics failed: {e}");
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
        _conn: &Connection,
        _qh: &QueueHandle<Self>,
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
                let name_prefix = state.name_prefix.clone();
                let Some(entry) = state.outputs.get_mut(&output_name) else {
                    log::warn!("configure for unknown output_name={output_name}");
                    return;
                };
                let scale = entry.scale.max(1);
                let f120 = entry.fractional_scale_120;
                let physical =
                    physical_output_size((width, height), scale, f120, entry.viewport.is_some());
                if let Some(surface) = entry.surface.as_ref() {
                    if f120 > 0 && entry.viewport.is_some() {
                        surface.set_buffer_scale(1);
                        if let Some(viewport) = entry.viewport.as_ref() {
                            viewport.set_destination(width as i32, height as i32);
                        }
                    } else {
                        surface.set_buffer_scale(scale);
                        if let Some(viewport) = entry.viewport.as_ref() {
                            viewport.set_destination(-1, -1);
                        }
                    }
                }
                if entry.binding.is_none() {
                    let Some(runtime) = state.vulkan.as_ref().cloned() else {
                        log::error!("output {output_name}: configure before Vulkan initialization");
                        return;
                    };
                    let Some(vk_surface) = entry.vk_surface.take() else {
                        log::error!("output {output_name}: configure without Vulkan surface");
                        return;
                    };
                    let presenter =
                        match vulkan::WsiPresenter::new(Arc::clone(&runtime), vk_surface, physical)
                        {
                            Ok(presenter) => presenter,
                            Err(error) => {
                                log::error!(
                                "output {output_name}: initialize WSI presenter failed: {error:#}"
                            );
                                return;
                            }
                        };
                    entry.binding = Some(make_output_binding(
                        &name_prefix,
                        entry,
                        output_name,
                        runtime,
                        presenter,
                        physical,
                        None,
                    ));
                }
                let binding = entry.binding.as_ref().expect("binding just created");
                {
                    let mut reg = state.binding_registry.lock().unwrap();
                    reg.insert(binding.display_name().to_string(), binding.watcher.clone());
                }
                binding.scale.store(scale, Ordering::SeqCst);
                binding.fractional_scale_120.store(f120, Ordering::SeqCst);
                binding.watcher.set_logical_size((width, height));
                entry.configured_size = Some(physical);
                *binding.configured_size.lock().unwrap() = Some(physical);
                binding
                    .presenter
                    .lock()
                    .unwrap()
                    .request_resize(physical.0, physical.1);
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
                    log::warn!("output {output_name}: push display metrics failed: {e}");
                }
            }
            zwlr_layer_surface_v1::Event::Closed => {
                log::warn!("output {output_name}: layer_surface closed by compositor");
                if let Some(entry) = state.outputs.get_mut(&output_name) {
                    if let Some(binding) = entry.binding.as_ref() {
                        state
                            .binding_registry
                            .lock()
                            .unwrap()
                            .remove(binding.display_name());
                    } else if let (Some(runtime), Some(surface)) =
                        (state.vulkan.as_ref(), entry.vk_surface.take())
                    {
                        runtime.destroy_surface(surface);
                    }
                    if let Some(session) = entry.session.take() {
                        if let Some(binding) = entry.binding.as_ref() {
                            shutdown_display_session(binding, session);
                        }
                    }
                    entry.surface = None;
                    entry.layer_surface = None;
                    entry.binding = None;
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

fn push_resize_if_registered(binding: &Rc<OutputBinding>, physical: (u32, u32)) -> Result<()> {
    if !binding.registered.load(Ordering::SeqCst) {
        return Ok(());
    }
    {
        let refresh_mhz = binding.refresh_mhz.load(Ordering::SeqCst);
        let last = binding.last_pushed_metrics.lock().unwrap();
        if *last == Some((physical.0, physical.1, refresh_mhz)) {
            return Ok(());
        }
    }
    let refresh_mhz = binding.refresh_mhz.load(Ordering::SeqCst);
    let metrics = sys::waywallen_display_metrics_t {
        width: physical.0,
        height: physical.1,
        refresh_mhz,
    };
    let rc = binding.with_display(|d| unsafe { sys::waywallen_display_set_metrics(d, &metrics) });
    if let Some(rc) = rc {
        if rc < 0 {
            return Err(anyhow!("waywallen_display_set_metrics: {rc}"));
        }
    } else {
        return Ok(());
    }
    *binding.last_pushed_metrics.lock().unwrap() = Some((physical.0, physical.1, refresh_mhz));
    log::info!(
        "[{}] pushed display metrics {}x{}@{}mHz",
        binding.display_name,
        physical.0,
        physical.1,
        refresh_mhz
    );
    Ok(())
}

fn start_display_session(sock: &Path, binding: &Rc<OutputBinding>) -> Result<DisplaySession> {
    let (width, height) = binding
        .configured_size
        .lock()
        .unwrap()
        .expect("display session started before configure");
    let display_name = CString::new(binding.display_name.as_str()).context("display name")?;
    let instance_id = CString::new(binding.instance_id.as_str()).context("instance id")?;
    let socket_path = CString::new(sock.as_os_str().as_encoded_bytes()).context("socket path")?;

    let callbacks = sys::waywallen_display_callbacks_t {
        on_binding_ready: Some(on_binding_ready),
        on_textures_releasing: Some(on_textures_releasing),
        on_composition_config: Some(on_composition_config),
        on_frame_ready: Some(on_frame_ready),
        on_presentation_snapshot: Some(on_presentation_snapshot),
        on_presentation_state: Some(on_presentation_state),
        on_disconnected: Some(on_disconnected),
        user_data: Rc::as_ptr(binding) as *mut c_void,
    };

    let display = unsafe { sys::waywallen_display_new(&callbacks) };
    if display.is_null() {
        bail!("waywallen_display_new failed");
    }
    {
        *binding.display.lock().unwrap() = Some(DisplayPtr(display));
    }
    let start = (|| -> Result<DisplaySession> {
        let context = binding.runtime.display_context();
        let rc = unsafe { sys::waywallen_display_bind_vulkan(display, &context) };
        if rc < 0 {
            bail!("waywallen_display_bind_vulkan failed: {rc}");
        }
        let presentation_caps = if binding.presenter.lock().unwrap().supports_pause_blur() {
            sys::WAYWALLEN_PRESENTATION_CAP_PAUSE_BLUR
        } else {
            0
        };
        let rc =
            unsafe { sys::waywallen_display_set_presentation_caps(display, presentation_caps) };
        if rc < 0 {
            bail!("waywallen_display_set_presentation_caps failed: {rc}");
        }
        let flags = binding.watcher.window_flags();
        let rc = unsafe { sys::waywallen_display_set_window_state(display, flags) };
        if rc < 0 {
            bail!("waywallen_display_set_window_state failed: {rc}");
        }
        let refresh_mhz = binding.refresh_mhz.load(Ordering::SeqCst);
        let metrics = sys::waywallen_display_metrics_t {
            width,
            height,
            refresh_mhz,
        };
        let rc = unsafe {
            sys::waywallen_display_begin_connect(
                display,
                socket_path.as_ptr(),
                display_name.as_ptr(),
                instance_id.as_ptr(),
                &metrics,
            )
        };
        if rc < 0 {
            bail!("waywallen_display_begin_connect failed: {rc}");
        }
        let mut session = DisplaySession {
            display: DisplayPtr(display),
            state: DisplaySessionState::Handshake {
                events: libc::POLLIN | libc::POLLOUT,
            },
            started: Instant::now(),
            _binding: Rc::clone(binding),
        };
        advance_display_handshake(binding, &mut session)?;
        if unsafe { sys::waywallen_display_get_fd(display) } < 0 {
            bail!("display session has no pollable fd after begin_connect");
        }
        Ok(session)
    })();
    if start.is_err() {
        binding.display.lock().unwrap().take();
        unsafe { sys::waywallen_display_shutdown(display) };
    }
    start
}

fn advance_display_handshake(
    binding: &Rc<OutputBinding>,
    session: &mut DisplaySession,
) -> Result<()> {
    loop {
        let rc = unsafe { sys::waywallen_display_advance_handshake(session.display.0) };
        match rc {
            sys::WAYWALLEN_HS_DONE => {
                session.state = DisplaySessionState::Ready;
                binding.registered.store(true, Ordering::SeqCst);
                if let Some((width, height)) = *binding.configured_size.lock().unwrap() {
                    let refresh_mhz = binding.refresh_mhz.load(Ordering::SeqCst);
                    *binding.last_pushed_metrics.lock().unwrap() =
                        Some((width, height, refresh_mhz));
                    let display_id =
                        unsafe { sys::waywallen_display_get_display_id(session.display.0) };
                    log::info!(
                        "[{}] registered as display_id={display_id} instance_id={} \
                         ({width}x{height}@{refresh_mhz}mHz)",
                        binding.display_name,
                        binding.instance_id,
                    );
                }
                return Ok(());
            }
            sys::WAYWALLEN_HS_NEED_READ => {
                session.state = DisplaySessionState::Handshake {
                    events: libc::POLLIN,
                };
                return Ok(());
            }
            sys::WAYWALLEN_HS_NEED_WRITE => {
                session.state = DisplaySessionState::Handshake {
                    events: libc::POLLOUT,
                };
                return Ok(());
            }
            sys::WAYWALLEN_HS_PROGRESS => continue,
            error if error < 0 => bail!("display handshake failed: {error}"),
            other => bail!("display handshake returned unexpected action: {other}"),
        }
    }
}

fn process_display_event(
    binding: &Rc<OutputBinding>,
    session: &mut DisplaySession,
    revents: i16,
) -> Result<()> {
    if !session.is_ready() {
        advance_display_handshake(binding, session)?;
        if revents & (libc::POLLERR | libc::POLLHUP | libc::POLLNVAL) != 0 && !session.is_ready() {
            bail!("display socket closed during handshake (poll revents=0x{revents:x})");
        }
        return Ok(());
    }
    if revents & libc::POLLOUT != 0 {
        let rc = unsafe { sys::waywallen_display_handle_writable(session.display.0) };
        if rc < 0 {
            bail!("waywallen_display_handle_writable failed: {rc}");
        }
    }
    if revents & (libc::POLLIN | libc::POLLERR | libc::POLLHUP) != 0 {
        let rc = unsafe { sys::waywallen_display_dispatch(session.display.0) };
        if rc < 0 {
            bail!("waywallen_display_dispatch failed: {rc}");
        }
        while unsafe { sys::waywallen_display_drain(session.display.0) } > 0 {}
        if unsafe { sys::waywallen_display_wants_writable(session.display.0) } {
            let rc = unsafe { sys::waywallen_display_handle_writable(session.display.0) };
            if rc < 0 {
                bail!("waywallen_display_handle_writable failed: {rc}");
            }
        }
    }
    if revents & (libc::POLLERR | libc::POLLHUP | libc::POLLNVAL) != 0 {
        bail!("display socket closed (poll revents=0x{revents:x})");
    }
    Ok(())
}

fn shutdown_display_session(binding: &Rc<OutputBinding>, session: DisplaySession) {
    binding.registered.store(false, Ordering::SeqCst);
    binding.pending_present.store(false, Ordering::SeqCst);
    binding.next_redraw.lock().unwrap().take();
    binding.last_pushed_metrics.lock().unwrap().take();
    binding.presenter.lock().unwrap().reset_display_session();
    unsafe { sys::waywallen_display_shutdown(session.display.0) };
    binding.display.lock().unwrap().take();
}

fn shutdown_output_entry(mut entry: OutputEntry) {
    if let (Some(binding), Some(session)) = (entry.binding.as_ref(), entry.session.take()) {
        shutdown_display_session(binding, session);
    }
}

unsafe extern "C" fn on_binding_ready(
    user_data: *mut c_void,
    raw_binding: *const sys::waywallen_binding_t,
) {
    let binding = binding_from_user_data(user_data);
    if raw_binding.is_null() {
        return;
    }
    let ready = &*raw_binding;
    let t = &ready.textures;
    if t.backend != sys::WAYWALLEN_BACKEND_VULKAN || t.count == 0 || t.vk_images.is_null() {
        log::warn!(
            "[{}] binding_ready without Vulkan images",
            binding.display_name
        );
        return;
    }
    log::info!(
        "[{}] Vulkan producer binding ready: generation={} count={} {}x{} fourcc=0x{:08x}",
        binding.display_name,
        t.buffer_generation,
        t.count,
        t.tex_width,
        t.tex_height,
        t.fourcc
    );
    let raw_images = std::slice::from_raw_parts(t.vk_images, t.count as usize);
    let images = raw_images
        .iter()
        .map(|image| vk::Image::from_raw(*image as usize as u64))
        .collect::<Vec<_>>();
    if let Err(error) = binding.presenter.lock().unwrap().install_direct_binding(
        t.buffer_generation,
        vk::Extent2D {
            width: t.tex_width,
            height: t.tex_height,
        },
        &images,
    ) {
        log::warn!(
            "[{}] install direct Vulkan binding failed: {error:#}",
            binding.display_name
        );
        return;
    }
    apply_composition_config(binding, &ready.config);
}

unsafe extern "C" fn on_textures_releasing(
    user_data: *mut c_void,
    _t: *const sys::waywallen_textures_t,
) {
    let binding = binding_from_user_data(user_data);
    let display = {
        let guard = binding.display.lock().unwrap();
        let Some(display) = guard.as_ref() else {
            return;
        };
        display.0
    };
    if let Err(error) = binding
        .presenter
        .lock()
        .unwrap()
        .retire_direct_binding(display)
    {
        log::error!(
            "[{}] retire direct Vulkan binding failed: {error:#}",
            binding.display_name
        );
    }
}

fn apply_composition_config(binding: &OutputBinding, c: &sys::waywallen_composition_config_t) {
    log::debug!(
        "[{}] composition source=({}, {}, {}, {}) destination=({}, {}, {}, {}) transform={} clear=({}, {}, {}, {})",
        binding.display_name,
        c.source_rect.x,
        c.source_rect.y,
        c.source_rect.w,
        c.source_rect.h,
        c.dest_rect.x,
        c.dest_rect.y,
        c.dest_rect.w,
        c.dest_rect.h,
        c.transform,
        c.clear_color.r,
        c.clear_color.g,
        c.clear_color.b,
        c.clear_color.a
    );
    let mut cfg = binding.config.lock().unwrap();
    cfg.source = [
        c.source_rect.x,
        c.source_rect.y,
        c.source_rect.w,
        c.source_rect.h,
    ];
    cfg.destination = [c.dest_rect.x, c.dest_rect.y, c.dest_rect.w, c.dest_rect.h];
    cfg.transform = c.transform;
    cfg.clear = [
        c.clear_color.r,
        c.clear_color.g,
        c.clear_color.b,
        c.clear_color.a,
    ];
}

unsafe extern "C" fn on_composition_config(
    user_data: *mut c_void,
    c: *const sys::waywallen_composition_config_t,
) {
    let binding = binding_from_user_data(user_data);
    if c.is_null() {
        return;
    }
    apply_composition_config(binding, &*c);
}

fn request_present(binding: &OutputBinding) {
    binding.next_redraw.lock().unwrap().take();
    binding.pending_present.store(true, Ordering::SeqCst);
}

unsafe extern "C" fn on_presentation_snapshot(
    user_data: *mut c_void,
    presentation: *const sys::waywallen_presentation_snapshot_t,
) {
    let binding = binding_from_user_data(user_data);
    if presentation.is_null() {
        return;
    }
    let presentation = &*presentation;
    if presentation.config.generation == 0 {
        binding.presenter.lock().unwrap().reset_display_session();
        binding.pending_present.store(false, Ordering::SeqCst);
        binding.next_redraw.lock().unwrap().take();
        return;
    }
    let pause = presentation.config.pause_effect;
    let target = vulkan::PausePresentation {
        configured: pause.kind
            == sys::waywallen_pause_effect_kind_t::WAYWALLEN_PAUSE_EFFECT_KIND_BLUR,
        active: presentation.state.pause_effect.active,
        radius: pause.blur.radius,
    };
    let changed = binding
        .presenter
        .lock()
        .unwrap()
        .apply_pause_snapshot(target, Instant::now());
    log::debug!(
        "[{}] Pause Effect snapshot cfg={} state={} configured={} active={} radius={}",
        binding.display_name,
        presentation.config.generation,
        presentation.state.generation,
        target.configured,
        target.active,
        target.radius
    );
    if changed {
        request_present(binding);
    }
}

unsafe extern "C" fn on_presentation_state(
    user_data: *mut c_void,
    state: *const sys::waywallen_presentation_state_t,
) {
    let binding = binding_from_user_data(user_data);
    if state.is_null() {
        return;
    }
    let state = &*state;
    let changed = binding
        .presenter
        .lock()
        .unwrap()
        .apply_pause_state(state.pause_effect.active, Instant::now());
    log::debug!(
        "[{}] Pause Effect state generation={} config={} active={}",
        binding.display_name,
        state.generation,
        state.config_generation,
        state.pause_effect.active
    );
    if changed {
        request_present(binding);
    }
}

unsafe extern "C" fn on_frame_ready(user_data: *mut c_void, f: *const sys::waywallen_frame_t) {
    let binding = binding_from_user_data(user_data);
    if f.is_null() {
        return;
    }
    let f = &*f;
    let display = {
        let guard = binding.display.lock().unwrap();
        let Some(display) = guard.as_ref() else {
            return;
        };
        display.0
    };
    let mut presenter = binding.presenter.lock().unwrap();
    if !presenter.has_direct_binding() {
        log::warn!(
            "[{}] skipping FrameReady generation={} index={} seq={}: direct Vulkan binding is not installed",
            binding.display_name,
            f.buffer_generation,
            f.buffer_index,
            f.seq
        );
        if let Err(release_error) = vulkan::discard_direct_frame(display, f) {
            log::warn!(
                "[{}] discard skipped frame seq={} failed: {release_error:#}",
                binding.display_name,
                f.seq
            );
        }
        return;
    }
    let mut direct = sys::waywallen_vk_direct_frame_t::default();
    let rc = sys::waywallen_display_vulkan_direct_frame(display, f, &mut direct);
    if rc != sys::WAYWALLEN_OK {
        if let Err(release_error) = vulkan::discard_direct_frame(display, f) {
            log::warn!(
                "[{}] discard unresolved direct frame seq={} failed: {release_error:#}",
                binding.display_name,
                f.seq
            );
        }
        log::warn!(
            "[{}] resolve direct Vulkan frame seq={} failed: {rc}",
            binding.display_name,
            f.seq
        );
        return;
    }
    if let Err(error) = presenter.enqueue_direct_frame(f, &direct) {
        if let Err(release_error) = vulkan::discard_direct_frame(display, f) {
            log::warn!(
                "[{}] discard rejected direct frame seq={} failed: {release_error:#}",
                binding.display_name,
                f.seq
            );
        }
        log::warn!(
            "[{}] queue direct frame seq={} failed: {error:#}",
            binding.display_name,
            f.seq
        );
        return;
    }
    drop(presenter);
    request_present(binding);
}

fn present_latest(binding: &OutputBinding) -> Result<()> {
    let config = *binding.config.lock().unwrap();
    let composition = vulkan::Composition {
        source: config.source,
        destination: config.destination,
        transform: config.transform,
        clear: config.clear,
    };
    let display = binding
        .display
        .lock()
        .unwrap()
        .as_ref()
        .ok_or_else(|| anyhow!("present requested without a display session"))?
        .0;
    let mut presenter = binding.presenter.lock().unwrap();
    match presenter.present(display, &composition, Instant::now())? {
        vulkan::PresentResult::Presented { redraw } => {
            binding.pending_present.store(false, Ordering::SeqCst);
            *binding.next_redraw.lock().unwrap() = redraw.then(|| {
                Instant::now() + redraw_interval(binding.refresh_mhz.load(Ordering::SeqCst))
            });
        }
        vulkan::PresentResult::Pending => {
            binding.pending_present.store(true, Ordering::SeqCst);
            binding.next_redraw.lock().unwrap().take();
        }
    }
    Ok(())
}

fn redraw_interval(refresh_mhz: u32) -> Duration {
    let period_ns =
        (1_000_000_000_000u64 / u64::from(refresh_mhz.max(1))).clamp(4_000_000, 33_000_000);
    Duration::from_nanos(period_ns)
}

unsafe extern "C" fn on_disconnected(user_data: *mut c_void, err: i32, msg: *const c_char) {
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

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

/// Run the layer-shell display backend. Connects to the Wayland compositor
/// and the daemon's UDS display socket, registers each output as a display.
/// Blocks until the compositor disconnects or an unrecoverable error occurs.
const GETTEXT_DOMAIN: &str = "waywallen-layer-shell";

fn init_gettext() {
    gettextrs::setlocale(gettextrs::LocaleCategory::LcAll, "");

    let locale_dir = std::env::var_os("WAYWALLEN_LOCALEDIR")
        .map(PathBuf::from)
        .or_else(|| {
            let exe_dir = std::env::current_exe().ok()?.parent()?.to_path_buf();
            let sibling = exe_dir.join("share/locale");
            if sibling.is_dir() {
                return Some(sibling);
            }
            let prefix = exe_dir.parent()?.join("share/locale");
            prefix.is_dir().then_some(prefix)
        });
    if let Some(dir) = locale_dir {
        if let Err(e) = gettextrs::bindtextdomain(GETTEXT_DOMAIN, dir) {
            log::debug!("bindtextdomain failed: {e}");
        }
    }
    if let Err(e) = gettextrs::bind_textdomain_codeset(GETTEXT_DOMAIN, "UTF-8") {
        log::debug!("bind_textdomain_codeset failed: {e}");
    }
    if let Err(e) = gettextrs::textdomain(GETTEXT_DOMAIN) {
        log::debug!("textdomain failed: {e}");
    }
}

fn usage() -> ! {
    eprintln!(
        "{}",
        gettextrs::gettext(
            "usage: waywallen-layer-shell [--socket PATH] [--name STR]\n\
             \n\
             Environment:\n\
               WAYWALLEN_SOCKET   fallback UDS path when --socket is omitted\n\
               WAYLAND_DISPLAY    required — picks the compositor to attach to"
        )
    );
    std::process::exit(2);
}

fn main() -> Result<()> {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info")).init();
    init_gettext();

    let mut socket: Option<PathBuf> = None;
    let mut name_prefix = String::from("output");
    let mut it = std::env::args().skip(1);
    while let Some(arg) = it.next() {
        match arg.as_str() {
            "--socket" => {
                socket = it.next().map(PathBuf::from);
                if socket.is_none() {
                    eprintln!("{}", gettextrs::gettext("--socket requires a value"));
                    usage();
                }
            }
            "--name" => {
                name_prefix = it.next().unwrap_or_else(|| {
                    eprintln!("{}", gettextrs::gettext("--name requires a value"));
                    usage();
                });
            }
            "-h" | "--help" => usage(),
            other => {
                eprintln!("{}: {other}", gettextrs::gettext("unknown argument"));
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
    let conn = Connection::connect_to_env().with_context(|| {
        gettextrs::gettext(
            "connect to WAYLAND_DISPLAY — are you running under a Wayland compositor?",
        )
    })?;
    let (globals, mut queue) = registry_queue_init::<App>(&conn).context("registry init")?;
    let qh: QueueHandle<App> = queue.handle();

    let (watcher_sender, watcher_commands) =
        watcher::command_channel().context("create watcher command channel")?;
    let mut app = App::new(socket, name_prefix, watcher_commands);

    watcher::spawn_all(app.binding_registry.clone());

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
                        session: None,
                        reconnect_at: Instant::now(),
                        reconnect_delay: INITIAL_RECONNECT_DELAY,
                        scale: 1,
                        fractional_scale: None,
                        fractional_scale_120: 0,
                        vk_surface: None,
                        configured_size: None,
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
        bail!(gettextrs::gettext(
            "compositor does not expose wl_compositor"
        ));
    }
    if app.layer_shell.is_none() {
        bail!(gettextrs::gettext(
            "compositor does not expose zwlr_layer_shell_v1 — try a different compositor (Hyprland/Sway/KWin/new Mutter)"
        ));
    }
    if app.outputs.is_empty() {
        bail!(gettextrs::gettext("no wl_output available"));
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
        let _ = app.bring_up_surface(name, &conn, &qh);
    }

    let wayland_surfaces: Vec<_> = app
        .outputs
        .iter()
        .filter_map(|(name, entry)| entry.surface.clone().map(|surface| (*name, surface)))
        .collect();
    let (runtime, vk_surfaces) = vulkan::VulkanRuntime::new(
        &conn,
        &wayland_surfaces,
        (app.compositor_drm_major, app.compositor_drm_minor),
    )?;
    for (name, surface) in vk_surfaces {
        if let Some(entry) = app.outputs.get_mut(&name) {
            entry.vk_surface = Some(surface);
        } else {
            runtime.destroy_surface(surface);
        }
    }
    app.vulkan = Some(runtime);

    loop {
        queue
            .dispatch_pending(&mut app)
            .context("dispatch pending Wayland events")?;
        app.drain_watcher_commands();
        app.retire_finished_sessions();
        app.start_due_sessions();
        app.pump_presenters();
        queue.flush().context("flush Wayland requests")?;

        let Some(read_guard) = queue.prepare_read() else {
            continue;
        };
        let display_sources = app.display_poll_sources();
        let mut poll_fds = Vec::with_capacity(2 + display_sources.len());
        poll_fds.push(libc::pollfd {
            fd: read_guard.connection_fd().as_raw_fd(),
            events: libc::POLLIN,
            revents: 0,
        });
        poll_fds.push(libc::pollfd {
            fd: app.watcher_commands.fd(),
            events: libc::POLLIN,
            revents: 0,
        });
        poll_fds.extend(display_sources.iter().map(|(_, poll_fd)| *poll_fd));

        let poll_result = unsafe {
            libc::poll(
                poll_fds.as_mut_ptr(),
                poll_fds.len() as libc::nfds_t,
                app.poll_timeout_ms(),
            )
        };
        if poll_result < 0 {
            let error = std::io::Error::last_os_error();
            drop(read_guard);
            if error.kind() == std::io::ErrorKind::Interrupted {
                continue;
            }
            return Err(error).context("poll layer-shell event sources");
        }

        let wayland_events = poll_fds[0].revents;
        if wayland_events & (libc::POLLIN | libc::POLLERR | libc::POLLHUP) != 0 {
            read_guard.read().context("read Wayland events")?;
        } else {
            drop(read_guard);
        }

        if poll_fds[1].revents & (libc::POLLIN | libc::POLLERR | libc::POLLHUP) != 0 {
            app.drain_watcher_commands();
        }
        for ((output_name, _), poll_fd) in display_sources
            .into_iter()
            .zip(poll_fds.into_iter().skip(2))
        {
            if poll_fd.revents != 0 {
                app.process_display_poll(output_name, poll_fd.revents);
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::{physical_output_size, redraw_interval};
    use std::time::Duration;

    #[test]
    fn fractional_output_size_rounds_to_nearest_physical_pixel() {
        assert_eq!(
            physical_output_size((1001, 801), 2, 150, true),
            (1251, 1001)
        );
        assert_eq!(
            physical_output_size((1001, 801), 2, 150, false),
            (2002, 1602)
        );
    }

    #[test]
    fn redraw_interval_tracks_refresh_rate_with_safe_bounds() {
        assert_eq!(redraw_interval(60_000), Duration::from_nanos(16_666_666));
        assert_eq!(redraw_interval(1_000_000), Duration::from_millis(4));
        assert_eq!(redraw_interval(0), Duration::from_millis(33));
    }
}
