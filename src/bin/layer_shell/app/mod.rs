mod input;
mod wayland;

use self::wayland::{make_output_binding, WaylandSystem};
use crate::{vulkan, watcher};
use anyhow::{anyhow, bail, Context, Result};
use std::collections::HashMap;
use std::os::fd::AsRawFd;
use std::path::{Path, PathBuf};
use std::rc::Rc;
use std::sync::atomic::Ordering;
use std::sync::Arc;
use std::time::{Duration, Instant};
use wayland_client::globals::registry_queue_init;
use wayland_client::protocol::wl_compositor::WlCompositor;
use wayland_client::protocol::wl_output::WlOutput;
use wayland_client::protocol::wl_pointer::WlPointer;
use wayland_client::protocol::wl_seat::WlSeat;
use wayland_client::protocol::wl_surface::WlSurface;
use wayland_client::{Connection, Proxy, QueueHandle};
use wayland_protocols::wp::cursor_shape::v1::client::wp_cursor_shape_manager_v1::WpCursorShapeManagerV1;
use wayland_protocols::wp::fractional_scale::v1::client::wp_fractional_scale_manager_v1::WpFractionalScaleManagerV1;
use wayland_protocols::wp::fractional_scale::v1::client::wp_fractional_scale_v1::WpFractionalScaleV1;
use wayland_protocols::wp::linux_dmabuf::zv1::client::zwp_linux_dmabuf_feedback_v1::ZwpLinuxDmabufFeedbackV1;
use wayland_protocols::wp::linux_dmabuf::zv1::client::zwp_linux_dmabuf_v1::ZwpLinuxDmabufV1;
use wayland_protocols::wp::viewporter::client::wp_viewport::WpViewport;
use wayland_protocols::wp::viewporter::client::wp_viewporter::WpViewporter;
use wayland_protocols_wlr::layer_shell::v1::client::zwlr_layer_shell_v1::{
    Layer, ZwlrLayerShellV1,
};
use wayland_protocols_wlr::layer_shell::v1::client::zwlr_layer_surface_v1::{
    Anchor, KeyboardInteractivity, ZwlrLayerSurfaceV1,
};
use waywallen_display::client::app::{
    present_latest, publish_window_state, shutdown_display_session, DisplaySession,
    DisplaySessionState, DisplaySessions, FrameConfig, OutputBinding, OutputSlot, SessionOutput,
    INITIAL_RECONNECT_DELAY,
};

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

impl SessionOutput for OutputEntry {
    fn binding(&self) -> Option<&Rc<OutputBinding>> {
        self.binding.as_ref()
    }

    fn session(&self) -> Option<&DisplaySession> {
        self.session.as_ref()
    }

    fn slot(&mut self) -> OutputSlot<'_> {
        OutputSlot {
            binding: self.binding.as_ref(),
            session: &mut self.session,
            reconnect_at: &mut self.reconnect_at,
            reconnect_delay: &mut self.reconnect_delay,
        }
    }
}

struct App {
    compositor: Option<WlCompositor>,
    cursor_shape_manager: Option<(u32, WpCursorShapeManagerV1)>,
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
    window_states: HashMap<String, Vec<watcher::Window>>,
    watcher_commands: watcher::CommandReceiver,
    window_observation_available: bool,
    vulkan: Option<Arc<vulkan::VulkanRuntime>>,
}

struct PointerCtx {
    pointer: WlPointer,
    focus_output: Option<u32>,
    last_x: f64,
    last_y: f64,
    axis_source: u32,
}

impl DisplaySessions for App {
    type Output = OutputEntry;

    fn outputs(&self) -> &HashMap<u32, OutputEntry> {
        &self.outputs
    }

    fn outputs_mut(&mut self) -> &mut HashMap<u32, OutputEntry> {
        &mut self.outputs
    }

    fn socket_path(&self) -> &Path {
        &self.uds_sock
    }

    fn window_observation_available(&self) -> bool {
        self.window_observation_available
    }
}

impl App {
    fn new(
        uds_sock: PathBuf,
        name_prefix: String,
        watcher_commands: watcher::CommandReceiver,
    ) -> Self {
        Self {
            compositor: None,
            cursor_shape_manager: None,
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
            window_states: HashMap::new(),
            watcher_commands,
            window_observation_available: false,
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
            match runtime.create_surface(&WaylandSystem(conn), &surface) {
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
            &WaylandSystem(conn),
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
}

impl App {
    fn drain_watcher_commands(&mut self) {
        for command in self.watcher_commands.drain() {
            match command {
                watcher::Command::WindowState {
                    display_name,
                    windows,
                } => {
                    self.window_states
                        .insert(display_name.clone(), windows.clone());
                    let target = self.outputs.values().find_map(|entry| {
                        let binding = entry.binding.as_ref()?;
                        (binding.display_name == display_name).then_some((binding, &entry.session))
                    });
                    let Some((binding, session)) = target else {
                        continue;
                    };
                    if !binding.watcher.replace_windows(windows) {
                        continue;
                    }
                    let Some(session) = session.as_ref() else {
                        continue;
                    };
                    if matches!(session.state, DisplaySessionState::Retiring { .. }) {
                        continue;
                    }
                    let flags = binding.watcher.window_flags();
                    let rc = publish_window_state(binding, session.display);
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
            entry.session.as_ref().is_some_and(|session| {
                matches!(session.state, DisplaySessionState::Retiring { .. })
            }) || entry
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

pub(super) fn run(socket: PathBuf, name_prefix: String) -> Result<()> {
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

    app.window_observation_available =
        watcher::spawn_all(app.binding_registry.clone(), watcher_sender);

    // Diagnostics aid: run the watchers without registering any display,
    // logging the window-state flags they would feed the daemon.
    if std::env::var_os("WAYWALLEN_WATCHER_PROBE").is_some() {
        log::info!("watcher probe mode: no displays will be registered");
        loop {
            for command in app.watcher_commands.drain() {
                match command {
                    watcher::Command::WindowState {
                        display_name,
                        windows,
                    } => log::info!(
                        "probe: {display_name} flags=0x{:x}",
                        windows.iter().fold(0, |flags, window| flags | window.flags)
                    ),
                }
            }
            std::thread::sleep(std::time::Duration::from_millis(200));
        }
    }

    for g in globals.contents().clone_list() {
        match g.interface.as_str() {
            "wp_cursor_shape_manager_v1" => {
                app.cursor_shape_manager = Some((
                    g.name,
                    globals
                        .registry()
                        .bind::<WpCursorShapeManagerV1, _, _>(g.name, 1, &qh, ()),
                ));
            }
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
        &WaylandSystem(&conn),
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
        let watcher_index = app.watcher_commands.fd().map(|fd| {
            let index = poll_fds.len();
            poll_fds.push(libc::pollfd {
                fd,
                events: libc::POLLIN,
                revents: 0,
            });
            index
        });
        let display_start = poll_fds.len();
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

        if let Some(index) = watcher_index {
            if poll_fds[index].revents & (libc::POLLIN | libc::POLLERR | libc::POLLHUP) != 0 {
                app.drain_watcher_commands();
            }
        }
        for ((output_name, _), poll_fd) in display_sources
            .into_iter()
            .zip(poll_fds.into_iter().skip(display_start))
        {
            if poll_fd.revents != 0 {
                app.process_display_poll(output_name, poll_fd.revents);
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::wayland::physical_output_size;

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
}
