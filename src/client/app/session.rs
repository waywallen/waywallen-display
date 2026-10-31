use super::presentation::{
    on_binding_ready, on_composition_config, on_disconnected, on_frame_ready,
    on_presentation_snapshot, on_presentation_state, on_textures_releasing,
};
use super::{
    DisplayPtr, DisplaySession, DisplaySessionState, OutputBinding, INITIAL_RECONNECT_DELAY,
    MAX_RECONNECT_DELAY, STABLE_SESSION_TIME,
};
use crate as sys;
use anyhow::{anyhow, bail, Context, Result};
use std::collections::HashMap;
use std::ffi::{c_void, CString};
use std::path::Path;
use std::rc::Rc;
use std::sync::atomic::Ordering;
use std::time::{Duration, Instant};

pub fn publish_window_state(binding: &OutputBinding, display: DisplayPtr) -> i32 {
    let (generation, flags) = binding.watcher.window_state();
    unsafe {
        match generation {
            Some(generation) => {
                sys::waywallen_display_set_window_observation_state(display.0, generation, flags)
            }
            None => sys::waywallen_display_set_window_state(display.0, flags),
        }
    }
}

unsafe extern "C" fn on_window_observation_config(
    user_data: *mut c_void,
    config: *const sys::waywallen_window_observation_config_t,
) {
    let binding = &*(user_data as *const OutputBinding);
    let config = &*config;
    binding
        .watcher
        .configure(Some(crate::client::watcher::WindowExclusions {
            generation: config.generation,
            application_ids: config.excluded_application_ids.to_vec(),
            titles: config.excluded_titles.to_vec(),
            application_id_patterns: config
                .excluded_application_id_patterns
                .to_vec()
                .iter()
                .map(|pattern| wildmatch::WildMatch::new(pattern))
                .collect(),
            title_patterns: config
                .excluded_title_patterns
                .to_vec()
                .iter()
                .map(|pattern| wildmatch::WildMatch::new(pattern))
                .collect(),
        }));
    if let Some(display) = *binding.display.lock().unwrap() {
        publish_window_state(binding, display);
    }
}

/// The display-session part of one entry in a client's output table.
pub struct OutputSlot<'a> {
    pub binding: Option<&'a Rc<OutputBinding>>,
    pub session: &'a mut Option<DisplaySession>,
    pub reconnect_at: &'a mut Instant,
    pub reconnect_delay: &'a mut Duration,
}

pub trait SessionOutput {
    fn binding(&self) -> Option<&Rc<OutputBinding>>;
    fn session(&self) -> Option<&DisplaySession>;
    fn slot(&mut self) -> OutputSlot<'_>;
}

/// Starts, polls and retires the display session of every output, with
/// reconnect backoff. Implemented by each client's app over its own outputs.
pub trait DisplaySessions {
    type Output: SessionOutput;

    fn outputs(&self) -> &HashMap<u32, Self::Output>;
    fn outputs_mut(&mut self) -> &mut HashMap<u32, Self::Output>;
    fn socket_path(&self) -> &Path;
    fn window_observation_available(&self) -> bool;

    fn start_due_sessions(&mut self) {
        let now = Instant::now();
        let due: Vec<u32> = self
            .outputs_mut()
            .iter_mut()
            .filter_map(|(name, entry)| {
                let entry = entry.slot();
                (entry.session.is_none() && entry.binding.is_some() && *entry.reconnect_at <= now)
                    .then_some(*name)
            })
            .collect();
        for output_name in due {
            let Some(binding) = self
                .outputs()
                .get(&output_name)
                .and_then(|entry| entry.binding())
                .cloned()
            else {
                continue;
            };
            match start_display_session(
                self.socket_path(),
                &binding,
                self.window_observation_available(),
            ) {
                Ok(session) => {
                    if let Some(entry) = self.outputs_mut().get_mut(&output_name) {
                        *entry.slot().session = Some(session);
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
                    schedule_reconnect(self.outputs_mut(), output_name, Duration::ZERO);
                }
            }
        }
    }

    fn finish_session(&mut self, output_name: u32, error: &anyhow::Error) {
        let Some(entry) = self.outputs_mut().get_mut(&output_name) else {
            return;
        };
        let entry = entry.slot();
        let Some(session) = entry.session.as_mut() else {
            return;
        };
        if matches!(session.state, DisplaySessionState::Retiring { .. }) {
            if let Some(binding) = entry.binding {
                log::warn!(
                    "[{}] black fallback presentation failed: {error:#}",
                    binding.display_name
                );
                binding.pending_present.store(false, Ordering::SeqCst);
                binding.next_redraw.lock().unwrap().take();
                binding.presenter.lock().unwrap().abandon_blank();
            }
            session.state = DisplaySessionState::Retiring {
                blank_required: false,
            };
            return;
        }
        if let Some(binding) = entry.binding {
            log::warn!(
                "[{}] display session error: {error:#}",
                binding.display_name
            );
            binding.registered.store(false, Ordering::SeqCst);
            binding.next_redraw.lock().unwrap().take();
            binding.last_pushed_metrics.lock().unwrap().take();
            let mut presenter = binding.presenter.lock().unwrap();
            if let Err(release_error) = presenter.discard_pending_direct_frame(None) {
                log::warn!(
                    "[{}] release pending frame after disconnect failed: {release_error:#}",
                    binding.display_name
                );
            }
            let already_blank = presenter.blank_committed();
            let requested = presenter.request_blank();
            drop(presenter);
            if !already_blank {
                binding.pending_present.store(true, Ordering::SeqCst);
            }
            if requested {
                log::info!("[{}] black fallback requested", binding.display_name);
            }
        }
        session.state = DisplaySessionState::Retiring {
            blank_required: true,
        };
    }

    fn retire_finished_sessions(&mut self) {
        let ready: Vec<u32> = self
            .outputs()
            .iter()
            .filter_map(|(name, entry)| {
                let session = entry.session()?;
                let DisplaySessionState::Retiring { blank_required } = session.state else {
                    return None;
                };
                let binding = entry.binding()?;
                let presenter = binding.presenter.lock().unwrap();
                if blank_required && !presenter.blank_committed() {
                    return None;
                }
                match presenter.frames_idle() {
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
            let Some(entry) = self.outputs_mut().get_mut(&output_name) else {
                continue;
            };
            let entry = entry.slot();
            let Some(session) = entry.session.take() else {
                continue;
            };
            let lived = session.started.elapsed();
            if let Some(binding) = entry.binding {
                shutdown_display_session(binding, session);
            }
            schedule_reconnect(self.outputs_mut(), output_name, lived);
        }
    }

    fn display_poll_sources(&self) -> Vec<(u32, libc::pollfd)> {
        self.outputs()
            .iter()
            .filter_map(|(name, entry)| {
                let session = entry.session()?;
                if matches!(session.state, DisplaySessionState::Retiring { .. }) {
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
                .outputs_mut()
                .get_mut(&output_name)
                .ok_or_else(|| anyhow!("poll result for removed output {output_name}"))?
                .slot();
            let binding = entry
                .binding
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
}

fn schedule_reconnect<O: SessionOutput>(
    outputs: &mut HashMap<u32, O>,
    output_name: u32,
    lived: Duration,
) {
    let Some(entry) = outputs.get_mut(&output_name) else {
        return;
    };
    let entry = entry.slot();
    if lived >= STABLE_SESSION_TIME {
        *entry.reconnect_delay = INITIAL_RECONNECT_DELAY;
    }
    let delay = *entry.reconnect_delay;
    *entry.reconnect_at = Instant::now() + delay;
    *entry.reconnect_delay = std::cmp::min(delay * 2, MAX_RECONNECT_DELAY);
    if let Some(binding) = entry.binding {
        log::debug!(
            "[{}] session lived {:?}; reconnecting in {:?}",
            binding.display_name,
            lived,
            delay
        );
    }
}

fn start_display_session(
    sock: &Path,
    binding: &Rc<OutputBinding>,
    observation_available: bool,
) -> Result<DisplaySession> {
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
        let (pause_effect_caps, transition_caps) = {
            let presenter = binding.presenter.lock().unwrap();
            let pause_effect = if presenter.supports_pause_blur() {
                sys::WAYWALLEN_PAUSE_EFFECT_CAP_BLUR
            } else {
                0
            };
            let transition = if presenter.supports_transitions() {
                sys::WAYWALLEN_TRANSITION_CAP_FADE
                    | sys::WAYWALLEN_TRANSITION_CAP_WIPE
                    | sys::WAYWALLEN_TRANSITION_CAP_GROW
            } else {
                0
            };
            (pause_effect, transition)
        };
        let rc =
            unsafe { sys::waywallen_display_set_pause_effect_caps(display, pause_effect_caps) };
        if rc < 0 {
            bail!("waywallen_display_set_pause_effect_caps failed: {rc}");
        }
        let rc = unsafe { sys::waywallen_display_set_transition_caps(display, transition_caps) };
        if rc < 0 {
            bail!("waywallen_display_set_transition_caps failed: {rc}");
        }
        binding.watcher.configure(None);
        if observation_available {
            let rc = unsafe {
                sys::waywallen_display_set_window_observation_callback(
                    display,
                    15,
                    Some(on_window_observation_config),
                    Rc::as_ptr(binding) as *mut c_void,
                )
            };
            if rc < 0 {
                bail!("waywallen_display_set_window_observation_callback failed: {rc}");
            }
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

pub fn shutdown_display_session(binding: &Rc<OutputBinding>, session: DisplaySession) {
    binding.registered.store(false, Ordering::SeqCst);
    binding.pending_present.store(false, Ordering::SeqCst);
    binding.next_redraw.lock().unwrap().take();
    binding.last_pushed_metrics.lock().unwrap().take();
    binding.presenter.lock().unwrap().reset_display_session();
    unsafe { sys::waywallen_display_shutdown(session.display.0) };
    binding.display.lock().unwrap().take();
}

pub fn shutdown_output_entry(mut entry: impl SessionOutput) {
    let entry = entry.slot();
    if let (Some(binding), Some(session)) = (entry.binding, entry.session.take()) {
        shutdown_display_session(binding, session);
    }
}
