use crate::OutputBinding;
use std::collections::HashMap;
use std::sync::{Arc, Mutex};

pub mod hyprland;
pub mod niri;
pub mod wayfire;
pub mod wlr;

pub type BindingRegistry = Arc<Mutex<HashMap<String, Arc<OutputBinding>>>>;

pub fn new_registry() -> BindingRegistry {
    Arc::new(Mutex::new(HashMap::new()))
}

/// Starts the one watcher that fits the compositor we are running under.
///
/// A compositor with its own IPC gets its own watcher: only that IPC can say
/// which workspace is visible, and every window on a hidden one has to stay out
/// of the count. [`wlr`] is the fallback for everything else.
pub fn spawn_all(registry: BindingRegistry) {
    if hyprland::detect_socket().is_some() {
        hyprland::spawn(registry)
    } else if niri::detect_socket().is_some() {
        niri::spawn(registry)
    } else if wayfire::detect_socket().is_some() {
        wayfire::spawn(registry)
    } else {
        wlr::spawn(registry)
    }
}

pub fn handle_return_code(
    watcher: &'static str,
    return_code: i32,
    flags: u32,
    binding: &Arc<OutputBinding>,
) {
    if return_code >= 0 {
        log::debug!(
            "{watcher}: [{}] window_state flags=0x{flags:x}",
            binding.display_name()
        );
    } else {
        log::warn!(
            "{watcher}: [{}] send window_state failed: {return_code}",
            binding.display_name()
        );
    }
}
