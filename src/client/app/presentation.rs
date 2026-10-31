use super::OutputBinding;
use crate as sys;
use crate::client::vulkan;
use anyhow::{anyhow, Result};
use ash::vk;
use ash::vk::Handle;
use std::ffi::{c_char, c_void, CStr};
use std::sync::atomic::Ordering;
use std::time::{Duration, Instant};

pub(super) unsafe extern "C" fn on_binding_ready(
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
        "[{}] Vulkan producer binding ready: generation={} count={} {}x{} fourcc=0x{:08x} content_token={} presentation_config={}",
        binding.display_name,
        t.buffer_generation,
        t.count,
        t.tex_width,
        t.tex_height,
        t.fourcc,
        ready.content_token,
        ready.presentation_config_generation
    );
    let raw_images = std::slice::from_raw_parts(t.vk_images, t.count as usize);
    let images = raw_images
        .iter()
        .map(|image| vk::Image::from_raw(*image as usize as u64))
        .collect::<Vec<_>>();
    let mut presenter = binding.presenter.lock().unwrap();
    if let Err(error) = presenter.install_direct_binding(
        t.buffer_generation,
        ready.content_token,
        ready.presentation_config_generation,
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
    drop(presenter);
    apply_composition_config(binding, &ready.config);
}

pub(super) unsafe extern "C" fn on_textures_releasing(
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
    let release_display = (sys::waywallen_display_conn_state(display)
        == sys::WAYWALLEN_CONN_CONNECTED)
        .then_some(display);
    if let Err(error) = binding
        .presenter
        .lock()
        .unwrap()
        .retire_direct_binding(release_display)
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

pub(super) unsafe extern "C" fn on_composition_config(
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

pub(super) unsafe extern "C" fn on_presentation_snapshot(
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
    let transition = transition_presentation(&presentation.config.transition);
    let changed = {
        let mut presenter = binding.presenter.lock().unwrap();
        let pause_changed = presenter.apply_pause_snapshot(target, Instant::now());
        presenter.apply_transition_snapshot(presentation.config.generation, transition)
            || pause_changed
    };
    log::debug!(
        "[{}] Pause Effect snapshot cfg={} state={} configured={} active={} radius={} transition={:?}",
        binding.display_name,
        presentation.config.generation,
        presentation.state.generation,
        target.configured,
        target.active,
        target.radius,
        transition
    );
    if changed {
        request_present(binding);
    }
}

fn transition_presentation(
    config: &sys::waywallen_transition_config_t,
) -> Option<vulkan::TransitionPresentation> {
    use sys::waywallen_transition_kind_t as Kind;
    let shape = match config.kind {
        Kind::WAYWALLEN_TRANSITION_KIND_NONE => return None,
        Kind::WAYWALLEN_TRANSITION_KIND_FADE => vulkan::TransitionShape::Fade,
        Kind::WAYWALLEN_TRANSITION_KIND_WIPE => vulkan::TransitionShape::Wipe {
            angle: config.angle,
        },
        Kind::WAYWALLEN_TRANSITION_KIND_GROW => vulkan::TransitionShape::Grow {
            origin: [config.origin_x, config.origin_y],
        },
    };
    Some(vulkan::TransitionPresentation {
        shape,
        duration: Duration::from_millis(u64::from(config.duration_ms)),
    })
}

pub(super) unsafe extern "C" fn on_presentation_state(
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

pub(super) unsafe extern "C" fn on_frame_ready(
    user_data: *mut c_void,
    f: *const sys::waywallen_frame_t,
) {
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
    if let Err(error) = presenter.replace_pending_direct_frame(display, f, &direct) {
        if let Err(release_error) = vulkan::discard_direct_frame(display, f) {
            log::warn!(
                "[{}] discard rejected direct frame seq={} failed: {release_error:#}",
                binding.display_name,
                f.seq
            );
        }
        log::warn!(
            "[{}] replace pending direct frame seq={} failed: {error:#}",
            binding.display_name,
            f.seq
        );
        return;
    }
    drop(presenter);
    request_present(binding);
}

pub fn present_latest(binding: &OutputBinding) -> Result<()> {
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
    let release_display = (unsafe { sys::waywallen_display_conn_state(display) }
        == sys::WAYWALLEN_CONN_CONNECTED)
        .then_some(display);
    let mut presenter = binding.presenter.lock().unwrap();
    let was_blank = presenter.blank_committed();
    match presenter.present(release_display, &composition, Instant::now())? {
        vulkan::PresentResult::Presented { redraw } => {
            if !was_blank && presenter.blank_committed() {
                log::info!("[{}] black fallback committed", binding.display_name);
            } else if was_blank && !presenter.blank_committed() {
                log::info!(
                    "[{}] first frame replaced black fallback",
                    binding.display_name
                );
            }
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

pub(super) fn redraw_interval(refresh_mhz: u32) -> Duration {
    let period_ns =
        (1_000_000_000_000u64 / u64::from(refresh_mhz.max(1))).clamp(4_000_000, 33_000_000);
    Duration::from_nanos(period_ns)
}

pub(super) unsafe extern "C" fn on_disconnected(
    user_data: *mut c_void,
    err: i32,
    msg: *const c_char,
) {
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
