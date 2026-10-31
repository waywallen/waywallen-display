mod input;
mod stacking;
mod x11;

use self::x11::{Atoms, Monitor};
use crate::{vulkan, watcher};
use anyhow::{anyhow, Context, Result};
use std::collections::HashMap;
use std::os::fd::AsRawFd;
use std::path::{Path, PathBuf};
use std::rc::Rc;
use std::sync::atomic::{AtomicBool, AtomicI32, AtomicU32, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};
use waywallen_display as sys;
use waywallen_display::client::app::{
    present_latest, publish_window_state, shutdown_display_session, shutdown_output_entry,
    DisplaySession, DisplaySessionState, DisplaySessions, FrameConfig, OutputBinding, OutputSlot,
    SessionOutput, INITIAL_RECONNECT_DELAY,
};
use x11rb::connection::Connection as _;
use x11rb::protocol::randr::{self, ConnectionExt as _};
use x11rb::protocol::xproto::{
    ChangeWindowAttributesAux, ConnectionExt as _, EventMask, Screen, Window,
};
use x11rb::protocol::Event;
use x11rb::xcb_ffi::XCBConnection;

struct OutputEntry {
    monitor: Monitor,
    binding: Option<Rc<OutputBinding>>,
    session: Option<DisplaySession>,
    reconnect_at: Instant,
    reconnect_delay: Duration,
    vk_surface: Option<ash::vk::SurfaceKHR>,
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
    screen: Screen,
    atoms: Atoms,
    server_drm: (u32, u32),
    /// Keyed by our own desktop window id.
    outputs: HashMap<u32, OutputEntry>,
    uds_sock: PathBuf,
    watcher_commands: watcher::CommandReceiver,
    window_states: HashMap<String, Vec<watcher::Window>>,
    window_observation_available: bool,
    stacking: stacking::Stacking,
    pointer: input::PointerForwarding,
    vulkan: Option<Arc<vulkan::VulkanRuntime>>,
    /// Declared last so it outlives every Vulkan surface built on it.
    conn: XCBConnection,
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
    fn xcb_target(&self) -> x11::XcbTarget {
        x11::XcbTarget {
            connection: self.conn.get_raw_xcb_connection(),
            visual: self.screen.root_visual,
        }
    }

    fn add_monitor(&mut self, monitor: Monitor) -> Result<Window> {
        let window = x11::create_desktop_window(&self.conn, &self.screen, &self.atoms, &monitor)?;
        log::info!(
            "monitor '{}': {}x{}+{}+{} @{}mHz -> desktop window 0x{window:x}",
            monitor.name,
            monitor.width,
            monitor.height,
            monitor.x,
            monitor.y,
            monitor.refresh_mhz
        );
        self.outputs.insert(
            window,
            OutputEntry {
                monitor,
                binding: None,
                session: None,
                reconnect_at: Instant::now(),
                reconnect_delay: INITIAL_RECONNECT_DELAY,
                vk_surface: None,
            },
        );
        Ok(window)
    }

    /// Builds the presenter and daemon binding for a window that already
    /// owns a Vulkan surface.
    fn bind_output(&mut self, window: Window) -> Result<()> {
        let runtime = self
            .vulkan
            .clone()
            .ok_or_else(|| anyhow!("bind output before Vulkan initialization"))?;
        let entry = self
            .outputs
            .get_mut(&window)
            .ok_or_else(|| anyhow!("bind unknown window 0x{window:x}"))?;
        let surface = entry
            .vk_surface
            .take()
            .ok_or_else(|| anyhow!("window 0x{window:x} has no Vulkan surface"))?;
        let physical = entry.monitor.size();
        let presenter = match vulkan::WsiPresenter::new(Arc::clone(&runtime), surface, physical) {
            Ok(presenter) => presenter,
            Err(error) => {
                return Err(error).context("initialize WSI presenter");
            }
        };
        let display_name = entry.monitor.name.clone();
        let instance_id = entry.monitor.instance_id();
        log::info!(
            "monitor '{display_name}': identity '{}' -> instance_id={instance_id}",
            entry.monitor.identity
        );
        let watcher = Arc::new(watcher::OutputInfo::new());
        if let Some(windows) = self.window_states.get(&display_name) {
            watcher.replace_windows(windows.clone());
        }
        entry.binding = Some(Rc::new(OutputBinding {
            display_name,
            instance_id,
            configured_size: Mutex::new(Some(physical)),
            scale: AtomicI32::new(1),
            fractional_scale_120: AtomicU32::new(0),
            refresh_mhz: AtomicU32::new(entry.monitor.refresh_mhz),
            display: Mutex::new(None),
            registered: AtomicBool::new(false),
            last_pushed_metrics: Mutex::new(None),
            config: Mutex::new(FrameConfig::default()),
            runtime,
            presenter: Mutex::new(presenter),
            pending_present: AtomicBool::new(false),
            next_redraw: Mutex::new(None),
            watcher,
        }));
        entry.reconnect_at = Instant::now();
        entry.reconnect_delay = INITIAL_RECONNECT_DELAY;
        Ok(())
    }

    fn remove_output(&mut self, window: Window) {
        let Some(mut entry) = self.outputs.remove(&window) else {
            return;
        };
        log::info!("monitor '{}' removed", entry.monitor.name);
        if entry.binding.is_none() {
            if let (Some(runtime), Some(surface)) = (self.vulkan.as_ref(), entry.vk_surface.take())
            {
                runtime.destroy_surface(surface);
            }
        }
        shutdown_output_entry(entry);
        if let Err(error) = self.conn.destroy_window(window) {
            log::warn!("destroy desktop window 0x{window:x}: {error}");
        }
    }

    /// Re-reads the RandR monitor list and reconciles the desktop windows
    /// with it: new monitors get a window, vanished ones lose theirs, and
    /// moved or resized ones are re-placed and re-announced to the daemon.
    fn sync_monitors(&mut self) -> Result<()> {
        let monitors = x11::query_monitors(&self.conn, &self.screen, &self.atoms)?;
        let gone: Vec<Window> = self
            .outputs
            .iter()
            .filter(|(_, entry)| !monitors.iter().any(|m| m.name == entry.monitor.name))
            .map(|(window, _)| *window)
            .collect();
        for window in gone {
            self.remove_output(window);
        }
        for monitor in monitors {
            let existing = self
                .outputs
                .iter_mut()
                .find(|(_, entry)| entry.monitor.name == monitor.name);
            match existing {
                Some((window, entry)) => {
                    if entry.monitor == monitor {
                        continue;
                    }
                    log::info!(
                        "monitor '{}' changed: {}x{}+{}+{} @{}mHz",
                        monitor.name,
                        monitor.width,
                        monitor.height,
                        monitor.x,
                        monitor.y,
                        monitor.refresh_mhz
                    );
                    let window = *window;
                    x11::place_desktop_window(&self.conn, window, &monitor)?;
                    let physical = monitor.size();
                    entry.monitor = monitor;
                    if let Some(binding) = entry.binding.as_ref() {
                        binding
                            .refresh_mhz
                            .store(entry.monitor.refresh_mhz, Ordering::SeqCst);
                        *binding.configured_size.lock().unwrap() = Some(physical);
                        binding
                            .presenter
                            .lock()
                            .unwrap()
                            .request_resize(physical.0, physical.1);
                        if let Err(error) = push_resize_if_registered(binding, physical) {
                            log::warn!(
                                "[{}] push display metrics failed: {error}",
                                binding.display_name
                            );
                        }
                    }
                }
                None => {
                    let name = monitor.name.clone();
                    let window = self.add_monitor(monitor)?;
                    let target = self.xcb_target();
                    let surface = match self.vulkan.as_ref() {
                        Some(runtime) => runtime.create_surface(&target, &window),
                        None => Err(anyhow!("no Vulkan runtime")),
                    };
                    match surface {
                        Ok(surface) => {
                            if let Some(entry) = self.outputs.get_mut(&window) {
                                entry.vk_surface = Some(surface);
                            }
                            if let Err(error) = self.bind_output(window) {
                                log::error!("monitor '{name}': {error:#}");
                            }
                        }
                        Err(error) => {
                            log::error!(
                                "monitor '{name}': current Vulkan device cannot present to it: \
                                 {error:#}"
                            );
                        }
                    }
                }
            }
        }
        Ok(())
    }

    fn handle_x11_event(&mut self, event: Event) {
        match event {
            Event::RandrScreenChangeNotify(_) | Event::RandrNotify(_) => {
                if let Err(error) = self.sync_monitors() {
                    log::warn!("RandR: re-reading monitors failed: {error:#}");
                }
            }
            Event::ConfigureNotify(event) => {
                if let Some(entry) = self.outputs.get(&event.window) {
                    let wanted = entry.monitor.size();
                    let actual = (u32::from(event.width), u32::from(event.height));
                    if actual != wanted {
                        log::warn!(
                            "monitor '{}': window manager resized the desktop window to {}x{} \
                             (wanted {}x{})",
                            entry.monitor.name,
                            actual.0,
                            actual.1,
                            wanted.0,
                            wanted.1
                        );
                    }
                }
            }
            Event::Expose(event) if event.count == 0 => {
                if let Some(binding) = self
                    .outputs
                    .get(&event.window)
                    .and_then(|entry| entry.binding.as_ref())
                {
                    if binding.registered.load(Ordering::SeqCst) {
                        binding.next_redraw.lock().unwrap().take();
                        binding.pending_present.store(true, Ordering::SeqCst);
                    }
                }
            }
            Event::PropertyNotify(event)
                if event.window == self.screen.root
                    && [
                        self.atoms._NET_CLIENT_LIST_STACKING,
                        self.atoms.CAJA_DESKTOP_WINDOW_ID,
                        self.atoms.NAUTILUS_DESKTOP_WINDOW_ID,
                    ]
                    .contains(&event.atom) =>
            {
                self.stacking.dirty_at = Some(event.time);
            }
            Event::XinputRawMotion(_) => self.pointer.moved = true,
            Event::MotionNotify(event) => self.handle_motion(&event),
            Event::ButtonPress(event) => self.handle_button(&event, true),
            Event::ButtonRelease(event) => self.handle_button(&event, false),
            Event::Error(error) => log::warn!("X11 error: {error:?}"),
            _ => {}
        }
    }

    fn drain_x11_events(&mut self) -> Result<()> {
        while let Some(event) = self.conn.poll_for_event().context("read X11 events")? {
            self.handle_x11_event(event);
        }
        Ok(())
    }

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
        if self.pointer.moved {
            return input::POINTER_INTERVAL_MS;
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
        self.vulkan.take();
    }
}

fn push_resize_if_registered(binding: &Rc<OutputBinding>, physical: (u32, u32)) -> Result<()> {
    if !binding.registered.load(Ordering::SeqCst) {
        return Ok(());
    }
    let refresh_mhz = binding.refresh_mhz.load(Ordering::SeqCst);
    if *binding.last_pushed_metrics.lock().unwrap() == Some((physical.0, physical.1, refresh_mhz)) {
        return Ok(());
    }
    let metrics = sys::waywallen_display_metrics_t {
        width: physical.0,
        height: physical.1,
        refresh_mhz,
    };
    let rc = binding.with_display(|d| unsafe { sys::waywallen_display_set_metrics(d, &metrics) });
    match rc {
        Some(rc) if rc < 0 => return Err(anyhow!("waywallen_display_set_metrics: {rc}")),
        Some(_) => {}
        None => return Ok(()),
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

pub(super) fn run(socket: PathBuf, _name_prefix: String) -> Result<()> {
    let (conn, screen_num) = XCBConnection::connect(None)
        .context("connect to DISPLAY — are you running under an X server?")?;
    let screen = conn.setup().roots[screen_num].clone();
    let atoms = Atoms::new(&conn)
        .context("intern atoms")?
        .reply()
        .context("intern atoms reply")?;
    conn.randr_select_input(
        screen.root,
        randr::NotifyMask::SCREEN_CHANGE | randr::NotifyMask::CRTC_CHANGE,
    )
    .context("RRSelectInput")?;
    // Root property changes drive the stacking checks; the window manager
    // owns the redirect bits, PropertyChange can be shared.
    conn.change_window_attributes(
        screen.root,
        &ChangeWindowAttributesAux::new().event_mask(EventMask::PROPERTY_CHANGE),
    )
    .context("select root PropertyNotify")?;
    input::select_raw_motion(&conn, screen.root);
    let server_drm = x11::server_drm_device(&conn, screen.root);

    let (watcher_sender, watcher_commands) =
        watcher::command_channel().context("create watcher command channel")?;
    let window_observation_available = watcher::x11::spawn(watcher_sender);
    let mut app = App {
        screen,
        atoms,
        server_drm,
        outputs: HashMap::new(),
        uds_sock: socket,
        watcher_commands,
        window_states: HashMap::new(),
        window_observation_available,
        stacking: stacking::Stacking::default(),
        pointer: input::PointerForwarding::default(),
        vulkan: None,
        conn,
    };

    let monitors = x11::query_monitors(&app.conn, &app.screen, &app.atoms)?;
    for monitor in monitors {
        app.add_monitor(monitor)?;
    }
    app.conn.flush().context("flush X11 requests")?;

    let windows: Vec<(u32, u32)> = app
        .outputs
        .keys()
        .map(|window| (*window, *window))
        .collect();
    let (runtime, vk_surfaces) =
        vulkan::VulkanRuntime::new(&app.xcb_target(), &windows, app.server_drm)?;
    for (window, surface) in vk_surfaces {
        if let Some(entry) = app.outputs.get_mut(&window) {
            entry.vk_surface = Some(surface);
        } else {
            runtime.destroy_surface(surface);
        }
    }
    app.vulkan = Some(runtime);
    let windows: Vec<u32> = app.outputs.keys().copied().collect();
    for window in windows {
        if let Err(error) = app.bind_output(window) {
            log::error!("desktop window 0x{window:x}: {error:#}");
        }
    }

    loop {
        app.drain_x11_events()?;
        app.drain_watcher_commands();
        app.retire_finished_sessions();
        app.start_due_sessions();
        app.sync_stacking();
        app.forward_pointer_over_icons();
        app.pump_presenters();
        app.conn.flush().context("flush X11 requests")?;
        // Replies read while flushing may have queued events behind them.
        app.drain_x11_events()?;

        let display_sources = app.display_poll_sources();
        let mut poll_fds = Vec::with_capacity(2 + display_sources.len());
        poll_fds.push(libc::pollfd {
            fd: app.conn.as_raw_fd(),
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
            if error.kind() == std::io::ErrorKind::Interrupted {
                continue;
            }
            return Err(error).context("poll X11 client event sources");
        }

        if poll_fds[0].revents & (libc::POLLERR | libc::POLLHUP) != 0 {
            return Err(anyhow!("X server connection closed"));
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
