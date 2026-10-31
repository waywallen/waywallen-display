use super::{App, FrameConfig, OutputBinding, OutputEntry, INITIAL_RECONNECT_DELAY};
use crate::{vulkan, watcher};
use anyhow::{anyhow, bail, Context, Result};
use ash::vk;
use md5::{Digest, Md5};
use std::ffi::CStr;
use std::os::unix::fs::FileExt;
use std::rc::Rc;
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Instant;
use wayland_client::delegate_noop;
use wayland_client::globals::GlobalListContents;
use wayland_client::protocol::wl_compositor::WlCompositor;
use wayland_client::protocol::wl_output;
use wayland_client::protocol::wl_output::WlOutput;
use wayland_client::protocol::wl_registry::WlRegistry;
use wayland_client::protocol::wl_seat::WlSeat;
use wayland_client::protocol::wl_surface::WlSurface;
use wayland_client::{Connection, Dispatch, Proxy, QueueHandle};
use wayland_protocols::wp::cursor_shape::v1::client::wp_cursor_shape_manager_v1::WpCursorShapeManagerV1;
use wayland_protocols::wp::fractional_scale::v1::client::wp_fractional_scale_manager_v1::WpFractionalScaleManagerV1;
use wayland_protocols::wp::fractional_scale::v1::client::wp_fractional_scale_v1::WpFractionalScaleV1;
use wayland_protocols::wp::fractional_scale::v1::client::{
    wp_fractional_scale_manager_v1, wp_fractional_scale_v1,
};
use wayland_protocols::wp::linux_dmabuf::zv1::client::zwp_linux_dmabuf_feedback_v1::ZwpLinuxDmabufFeedbackV1;
use wayland_protocols::wp::linux_dmabuf::zv1::client::zwp_linux_dmabuf_v1::ZwpLinuxDmabufV1;
use wayland_protocols::wp::linux_dmabuf::zv1::client::{
    zwp_linux_dmabuf_feedback_v1, zwp_linux_dmabuf_v1,
};
use wayland_protocols::wp::viewporter::client::wp_viewport::WpViewport;
use wayland_protocols::wp::viewporter::client::wp_viewporter::WpViewporter;
use wayland_protocols::wp::viewporter::client::{wp_viewport, wp_viewporter};
use wayland_protocols_wlr::layer_shell::v1::client::zwlr_layer_shell_v1::ZwlrLayerShellV1;
use wayland_protocols_wlr::layer_shell::v1::client::zwlr_layer_surface_v1::ZwlrLayerSurfaceV1;
use wayland_protocols_wlr::layer_shell::v1::client::{zwlr_layer_shell_v1, zwlr_layer_surface_v1};
use waywallen_display as sys;
use waywallen_display::client::app::{shutdown_display_session, shutdown_output_entry};

delegate_noop!(App: ignore WpCursorShapeManagerV1);

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
                } else if interface == "wp_cursor_shape_manager_v1"
                    && state.cursor_shape_manager.is_none()
                {
                    state.cursor_shape_manager = Some((
                        name,
                        registry.bind::<WpCursorShapeManagerV1, _, _>(name, 1, qh, ()),
                    ));
                } else if interface == "wl_seat" {
                    registry.bind::<WlSeat, _, _>(name, version.min(5), qh, name);
                    log::info!("hot-plug: wl_seat name={name} added");
                }
            }
            Event::GlobalRemove { name } => {
                if state.cursor_shape_manager.as_ref().map(|(id, _)| *id) == Some(name) {
                    if let Some((_, manager)) = state.cursor_shape_manager.take() {
                        manager.destroy();
                    }
                }
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

pub(super) fn make_output_binding(
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
    let watcher = existing_watcher.unwrap_or_else(|| Arc::new(watcher::OutputInfo::new()));
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

pub(super) fn physical_output_size(
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
                    if let Some(binding) = entry.binding.as_ref() {
                        if let Some(windows) = state.window_states.get(binding.display_name()) {
                            binding.watcher.replace_windows(windows.clone());
                        }
                    }
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

/// The Wayland connection as seen by the shared Vulkan presenter.
pub(super) struct WaylandSystem<'a>(pub(super) &'a Connection);

impl vulkan::WindowSystem for WaylandSystem<'_> {
    type Target = WlSurface;
    const NAME: &'static str = "Wayland";
    const APPLICATION: &'static CStr = c"waywallen-layer-shell";
    const SURFACE_EXTENSION: &'static CStr = ash::khr::wayland_surface::NAME;

    fn create_surface(
        &self,
        entry: &ash::Entry,
        instance: &ash::Instance,
        surface: &WlSurface,
    ) -> Result<vk::SurfaceKHR> {
        let display = self.0.backend().display_ptr();
        let wl_surface = surface.id().as_ptr();
        if display.is_null() || wl_surface.is_null() {
            bail!("wayland-client did not expose system display/surface handles");
        }
        let info = vk::WaylandSurfaceCreateInfoKHR::default()
            .display(display.cast())
            .surface(wl_surface.cast());
        let loader = ash::khr::wayland_surface::Instance::new(entry, instance);
        unsafe { loader.create_wayland_surface(&info, None) }.context("vkCreateWaylandSurfaceKHR")
    }

    fn presentation_support(
        &self,
        entry: &ash::Entry,
        instance: &ash::Instance,
        physical_device: vk::PhysicalDevice,
        queue_family: u32,
    ) -> bool {
        let display: *mut vk::wl_display = self.0.backend().display_ptr().cast();
        let loader = ash::khr::wayland_surface::Instance::new(entry, instance);
        unsafe {
            loader.get_physical_device_wayland_presentation_support(
                physical_device,
                queue_family,
                &mut *display,
            )
        }
    }
}
