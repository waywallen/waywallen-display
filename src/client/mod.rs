//! Code shared by the `waywallen-layer-shell` and `waywallen-x11` clients:
//! the Vulkan presenter, the display session and the window-state
//! bookkeeping. Internal to the clients, not a stable API.

pub mod app;
pub mod vulkan;
pub mod watcher;
