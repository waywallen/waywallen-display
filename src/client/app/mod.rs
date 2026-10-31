mod presentation;
mod session;

pub use self::presentation::present_latest;
pub use self::session::{
    publish_window_state, shutdown_display_session, shutdown_output_entry, DisplaySessions,
    OutputSlot, SessionOutput,
};

use crate as sys;
use crate::client::{vulkan, watcher};
use std::rc::Rc;
use std::sync::atomic::{AtomicBool, AtomicI32, AtomicU32};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

pub struct OutputBinding {
    pub display_name: String,
    pub instance_id: String,
    pub configured_size: Mutex<Option<(u32, u32)>>,
    pub scale: AtomicI32,
    pub fractional_scale_120: AtomicU32,
    pub refresh_mhz: AtomicU32,
    pub display: Mutex<Option<DisplayPtr>>,
    pub registered: AtomicBool,
    pub last_pushed_metrics: Mutex<Option<(u32, u32, u32)>>,
    pub config: Mutex<FrameConfig>,
    pub runtime: Arc<vulkan::VulkanRuntime>,
    pub presenter: Mutex<vulkan::WsiPresenter>,
    pub pending_present: AtomicBool,
    pub next_redraw: Mutex<Option<Instant>>,
    pub watcher: Arc<watcher::OutputInfo>,
}

impl OutputBinding {
    pub fn display_name(&self) -> &str {
        &self.display_name
    }
    pub fn with_display<F>(&self, f: F) -> Option<i32>
    where
        F: FnOnce(*mut sys::waywallen_display_t) -> i32,
    {
        let guard = self.display.lock().unwrap();
        guard.as_ref().map(|d| f(d.0))
    }
}

#[derive(Copy, Clone)]
pub struct DisplayPtr(pub *mut sys::waywallen_display_t);

pub const INITIAL_RECONNECT_DELAY: Duration = Duration::from_secs(2);
const MAX_RECONNECT_DELAY: Duration = Duration::from_secs(30);
const STABLE_SESSION_TIME: Duration = Duration::from_secs(20);

#[derive(Clone, Copy)]
pub enum DisplaySessionState {
    Handshake { events: i16 },
    Ready,
    Retiring { blank_required: bool },
}

pub struct DisplaySession {
    pub display: DisplayPtr,
    pub state: DisplaySessionState,
    started: Instant,
    _binding: Rc<OutputBinding>,
}

impl DisplaySession {
    pub fn is_ready(&self) -> bool {
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
            DisplaySessionState::Retiring { .. } => 0,
        }
    }
}

#[derive(Clone, Copy)]
pub struct FrameConfig {
    pub source: [f32; 4],
    pub destination: [f32; 4],
    pub transform: u32,
    pub clear: [f32; 4],
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

#[cfg(test)]
mod tests {
    use super::presentation::redraw_interval;
    use std::time::Duration;

    #[test]
    fn redraw_interval_tracks_refresh_rate_with_safe_bounds() {
        assert_eq!(redraw_interval(60_000), Duration::from_nanos(16_666_666));
        assert_eq!(redraw_interval(1_000_000), Duration::from_millis(4));
        assert_eq!(redraw_interval(0), Duration::from_millis(33));
    }
}
