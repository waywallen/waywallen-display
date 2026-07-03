use crate::OutputBinding;
use std::collections::HashMap;
use std::sync::{Arc, Mutex};

pub mod hyprland;
pub mod niri;
pub mod wayfire;

pub type BindingRegistry = Arc<Mutex<HashMap<String, Arc<OutputBinding>>>>;

pub fn new_registry() -> BindingRegistry {
    Arc::new(Mutex::new(HashMap::new()))
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
