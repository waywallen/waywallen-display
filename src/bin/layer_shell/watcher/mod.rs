use std::collections::HashMap;
use std::sync::{Arc, Mutex};

pub use waywallen_display::client::watcher::*;

pub mod cosmic;
pub mod cosmic_toplevel_info;
pub mod hyprland;
pub mod niri;
pub mod wayfire;
pub mod wlr;

pub type BindingRegistry = Arc<Mutex<HashMap<String, Arc<OutputInfo>>>>;

pub fn new_registry() -> BindingRegistry {
    Arc::new(Mutex::new(HashMap::new()))
}

/// Starts the one watcher that fits the compositor we are running under.
///
/// A compositor with its own IPC gets its own watcher: only that IPC can say
/// which workspace is visible, and every window on a hidden one has to stay out
/// of the count. [`cosmic`] reads the same knowledge off COSMIC's own
/// protocols, and [`wlr`] is the fallback for everything else.
pub fn spawn_all(registry: BindingRegistry, commands: CommandSender) -> bool {
    if hyprland::detect_socket().is_some() {
        hyprland::spawn(registry, commands)
    } else if niri::detect_socket().is_some() {
        niri::spawn(registry, commands)
    } else if wayfire::detect_socket().is_some() {
        wayfire::spawn(registry, commands)
    } else if cosmic::detect() {
        cosmic::spawn(commands);
        true
    } else {
        wlr::spawn(commands)
    }
}
