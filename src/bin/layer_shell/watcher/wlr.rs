//! Generic window-state watcher on `zwlr_foreign_toplevel_management_v1`.
//!
//! One watcher for every compositor that speaks the protocol but ships no IPC
//! of its own — Sway, river, labwc, … The handle `state` enum maps onto the
//! `WAYWALLEN_WIN_HAS_*` bits one for one, and `output_enter` / `output_leave`
//! give the per-display binding.
//!
//! What this cannot do is ask which workspace is visible — the protocol has no
//! such concept. How much that costs depends on the compositor:
//!
//! * Sway drops the window off the output (`output_leave`) as soon as its
//!   workspace is hidden and puts it back on the way in, so the accounting
//!   stays exact.
//! * labwc and niri keep the window on the output and only clear `activated`,
//!   so a window parked on a hidden desktop still contributes
//!   `HAS_NON_MINIMIZED`, `HAS_MAXIMIZED` and `HAS_FULLSCREEN`.
//!
//! That is why [`crate::watcher::spawn_all`] hands the compositor to its own
//! watcher whenever there is one and only falls back to this.

use crate::watcher::{handle_return_code, BindingRegistry};
use crate::OutputBinding;
use std::collections::{HashMap, HashSet};
use std::sync::atomic::Ordering;
use std::sync::Arc;
use std::thread;
use wayland_client::backend::ObjectId;
use wayland_client::globals::{registry_queue_init, GlobalListContents};
use wayland_client::protocol::wl_output::{self, WlOutput};
use wayland_client::protocol::wl_registry::{self, WlRegistry};
use wayland_client::{event_created_child, Connection, Dispatch, EventQueue, Proxy, QueueHandle};
use wayland_protocols_wlr::foreign_toplevel::v1::client::{
    zwlr_foreign_toplevel_handle_v1::{self, State, ZwlrForeignToplevelHandleV1},
    zwlr_foreign_toplevel_manager_v1::{self, ZwlrForeignToplevelManagerV1},
};
use waywallen_display::{
    WAYWALLEN_WIN_HAS_ACTIVE, WAYWALLEN_WIN_HAS_FULLSCREEN, WAYWALLEN_WIN_HAS_MAXIMIZED,
    WAYWALLEN_WIN_HAS_NON_MINIMIZED,
};

const MANAGER_INTERFACE: &str = "zwlr_foreign_toplevel_manager_v1";

/// Version 2 added the `fullscreen` state, version 3 the `parent` event; a
/// version 1 compositor still drives the other three bits.
const MANAGER_VERSION: u32 = 3;

/// `wl_output.name` carries the connector string the daemon knows each display
/// by, and it needs version 4.
const OUTPUT_VERSION: u32 = 4;

pub fn spawn(registry: BindingRegistry) {
    let conn = match Connection::connect_to_env() {
        Ok(conn) => conn,
        Err(error) => {
            log::error!("wlr_watcher: connect to compositor: {error}");
            return;
        }
    };
    let (globals, queue) = match registry_queue_init::<Watcher>(&conn) {
        Ok(pair) => pair,
        Err(error) => {
            log::error!("wlr_watcher: registry init: {error}");
            return;
        }
    };
    let qh = queue.handle();
    let mut watcher = Watcher::new(registry);
    let mut manager = None;
    for global in globals.contents().clone_list() {
        match global.interface.as_str() {
            MANAGER_INTERFACE => {
                manager = Some(
                    globals
                        .registry()
                        .bind::<ZwlrForeignToplevelManagerV1, _, _>(
                            global.name,
                            global.version.min(MANAGER_VERSION),
                            &qh,
                            (),
                        ),
                )
            }
            "wl_output" => {
                watcher.bind_output(globals.registry(), global.name, global.version, &qh)
            }
            _ => {}
        }
    }
    let Some(manager) = manager else {
        log::debug!("wlr_watcher: compositor does not expose {MANAGER_INTERFACE}");
        return;
    };
    log::info!(
        "wlr_watcher: enabled ({MANAGER_INTERFACE} v{})",
        manager.version()
    );
    thread::spawn(move || run_loop(queue, watcher, manager));
}

fn run_loop(
    mut queue: EventQueue<Watcher>,
    mut watcher: Watcher,
    _manager: ZwlrForeignToplevelManagerV1,
) {
    // The compositor announces every window it already has, so the first
    // roundtrip is what fills in the state the daemon starts out with.
    if let Err(error) = queue.roundtrip(&mut watcher) {
        log::error!("wlr_watcher: initial roundtrip: {error}");
        return;
    }
    watcher.push_state();
    loop {
        if let Err(error) = queue.blocking_dispatch(&mut watcher) {
            log::error!("wlr_watcher: dispatch: {error}");
            return;
        }
        if std::mem::take(&mut watcher.dirty) {
            watcher.push_state();
        }
    }
}

struct Watcher {
    registry: BindingRegistry,
    outputs: HashMap<ObjectId, Output>,
    toplevels: HashMap<ObjectId, Toplevel>,
    dirty: bool,
}

struct Output {
    global: u32,
    display_name: Option<String>,
}

#[derive(Default)]
struct Toplevel {
    outputs: HashSet<ObjectId>,
    state: WindowState,
    pending_outputs: HashSet<ObjectId>,
    pending_state: WindowState,
}

impl Toplevel {
    /// `output_enter`, `output_leave` and `state` are double-buffered, so
    /// nothing is published before `done` — Sway sends a spurious leave/enter
    /// pair for the same output on every fullscreen toggle, and labwc sends two
    /// `state` events in one batch when a window is minimized.
    fn commit(&mut self) -> bool {
        if self.outputs == self.pending_outputs && self.state == self.pending_state {
            return false;
        }
        self.outputs.clone_from(&self.pending_outputs);
        self.state = self.pending_state;
        true
    }
}

#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
struct WindowState {
    maximized: bool,
    minimized: bool,
    activated: bool,
    fullscreen: bool,
}

impl WindowState {
    /// The payload is a `wl_array` of `zwlr_foreign_toplevel_handle_v1.state`
    /// values, and every event replaces the previous state wholesale.
    fn from_event(payload: &[u8]) -> Self {
        let mut state = Self::default();
        for value in payload.chunks_exact(4) {
            let value = u32::from_ne_bytes([value[0], value[1], value[2], value[3]]);
            match State::try_from(value) {
                Ok(State::Maximized) => state.maximized = true,
                Ok(State::Minimized) => state.minimized = true,
                Ok(State::Activated) => state.activated = true,
                Ok(State::Fullscreen) => state.fullscreen = true,
                _ => log::debug!("wlr_watcher: unknown toplevel state {value}"),
            }
        }
        state
    }

    /// A minimized window is off screen, so it contributes nothing — not even
    /// `activated`, which labwc keeps set for one event after iconifying.
    fn to_flags(self) -> u32 {
        if self.minimized {
            return 0;
        }
        let mut flags = WAYWALLEN_WIN_HAS_NON_MINIMIZED;
        if self.activated {
            flags |= WAYWALLEN_WIN_HAS_ACTIVE
        }
        if self.maximized {
            flags |= WAYWALLEN_WIN_HAS_MAXIMIZED
        }
        if self.fullscreen {
            flags |= WAYWALLEN_WIN_HAS_FULLSCREEN
        }
        flags
    }
}

impl Watcher {
    fn new(registry: BindingRegistry) -> Self {
        Self {
            registry,
            outputs: HashMap::new(),
            toplevels: HashMap::new(),
            dirty: false,
        }
    }

    fn bind_output(
        &mut self,
        registry: &WlRegistry,
        global: u32,
        version: u32,
        qh: &QueueHandle<Self>,
    ) {
        if version < OUTPUT_VERSION {
            log::warn!(
                "wlr_watcher: wl_output v{version} has no name event, \
                 display {global} cannot be matched"
            );
            return;
        }
        let output = registry.bind::<WlOutput, _, _>(global, OUTPUT_VERSION, qh, ());
        self.outputs.insert(
            output.id(),
            Output {
                global,
                display_name: None,
            },
        );
    }

    fn drop_output(&mut self, global: u32) {
        self.outputs.retain(|_, output| output.global != global);
        self.dirty = true;
    }

    fn aggregate_flags(&self) -> HashMap<&str, u32> {
        let mut by_output: HashMap<&str, u32> = HashMap::new();
        for toplevel in self.toplevels.values() {
            let flags = toplevel.state.to_flags();
            if flags == 0 {
                continue;
            }
            for output in &toplevel.outputs {
                let Some(display_name) = self
                    .outputs
                    .get(output)
                    .and_then(|output| output.display_name.as_deref())
                else {
                    continue;
                };
                *by_output.entry(display_name).or_insert(0) |= flags;
            }
        }
        by_output
    }

    fn push_state(&self) {
        let by_output = self.aggregate_flags();
        let bindings: Vec<Arc<OutputBinding>> = match self.registry.lock() {
            Ok(registry) => registry.values().cloned().collect(),
            Err(error) => {
                log::error!("wlr_watcher: lock registry: {error}");
                return;
            }
        };
        for binding in bindings {
            let flags = by_output.get(binding.display_name()).copied().unwrap_or(0);
            if binding.window_flags().swap(flags, Ordering::SeqCst) == flags {
                continue;
            }
            log::debug!("wlr_watcher: {} flags: {flags}", binding.display_name());
            if !binding.is_registered() {
                continue;
            }
            let return_code = binding.with_display(|display| unsafe {
                waywallen_display::waywallen_display_set_window_state(display, flags)
            });
            if let Some(return_code) = return_code {
                handle_return_code("wlr_watcher", return_code, flags, &binding);
            }
        }
    }
}

impl Dispatch<WlRegistry, GlobalListContents> for Watcher {
    fn event(
        state: &mut Self,
        registry: &WlRegistry,
        event: wl_registry::Event,
        _: &GlobalListContents,
        _: &Connection,
        qh: &QueueHandle<Self>,
    ) {
        match event {
            wl_registry::Event::Global {
                name,
                interface,
                version,
            } if interface == "wl_output" => state.bind_output(registry, name, version, qh),
            wl_registry::Event::GlobalRemove { name } => state.drop_output(name),
            _ => {}
        }
    }
}

impl Dispatch<WlOutput, ()> for Watcher {
    fn event(
        state: &mut Self,
        output: &WlOutput,
        event: wl_output::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        let wl_output::Event::Name { name } = event else {
            return;
        };
        let Some(entry) = state.outputs.get_mut(&output.id()) else {
            return;
        };
        log::debug!("wlr_watcher: wl_output {} is '{name}'", entry.global);
        entry.display_name = Some(name);
        state.dirty = true;
    }
}

impl Dispatch<ZwlrForeignToplevelManagerV1, ()> for Watcher {
    fn event(
        state: &mut Self,
        _: &ZwlrForeignToplevelManagerV1,
        event: zwlr_foreign_toplevel_manager_v1::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        match event {
            zwlr_foreign_toplevel_manager_v1::Event::Toplevel { toplevel } => {
                state.toplevels.insert(toplevel.id(), Toplevel::default());
            }
            zwlr_foreign_toplevel_manager_v1::Event::Finished => {
                log::info!("wlr_watcher: compositor stopped the toplevel manager");
                state.toplevels.clear();
                state.dirty = true;
            }
            _ => {}
        }
    }

    event_created_child!(Watcher, ZwlrForeignToplevelManagerV1, [
        zwlr_foreign_toplevel_manager_v1::EVT_TOPLEVEL_OPCODE => (ZwlrForeignToplevelHandleV1, ()),
    ]);
}

impl Dispatch<ZwlrForeignToplevelHandleV1, ()> for Watcher {
    fn event(
        state: &mut Self,
        handle: &ZwlrForeignToplevelHandleV1,
        event: zwlr_foreign_toplevel_handle_v1::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        let handle_id = handle.id();
        if let zwlr_foreign_toplevel_handle_v1::Event::Closed = event {
            handle.destroy();
            state.toplevels.remove(&handle_id);
            state.dirty = true;
            return;
        }
        let Some(toplevel) = state.toplevels.get_mut(&handle_id) else {
            return;
        };
        let changed = match event {
            zwlr_foreign_toplevel_handle_v1::Event::OutputEnter { output } => {
                toplevel.pending_outputs.insert(output.id());
                false
            }
            zwlr_foreign_toplevel_handle_v1::Event::OutputLeave { output } => {
                toplevel.pending_outputs.remove(&output.id());
                false
            }
            zwlr_foreign_toplevel_handle_v1::Event::State { state: reported } => {
                toplevel.pending_state = WindowState::from_event(&reported);
                false
            }
            zwlr_foreign_toplevel_handle_v1::Event::Done => {
                let changed = toplevel.commit();
                if changed {
                    log::debug!(
                        "wlr_watcher: toplevel {handle_id} flags {} on {} output(s)",
                        toplevel.state.to_flags(),
                        toplevel.outputs.len()
                    );
                }
                changed
            }
            _ => false,
        };
        state.dirty |= changed;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn payload(states: &[State]) -> Vec<u8> {
        states
            .iter()
            .flat_map(|state| (*state as u32).to_ne_bytes())
            .collect()
    }

    #[test]
    fn empty_state_is_a_plain_window() {
        let state = WindowState::from_event(&payload(&[]));
        assert_eq!(state, WindowState::default());
        assert_eq!(state.to_flags(), WAYWALLEN_WIN_HAS_NON_MINIMIZED);
    }

    #[test]
    fn every_state_maps_onto_its_bit() {
        assert_eq!(
            WindowState::from_event(&payload(&[State::Activated])).to_flags(),
            WAYWALLEN_WIN_HAS_NON_MINIMIZED | WAYWALLEN_WIN_HAS_ACTIVE
        );
        assert_eq!(
            WindowState::from_event(&payload(&[State::Maximized, State::Activated])).to_flags(),
            WAYWALLEN_WIN_HAS_NON_MINIMIZED
                | WAYWALLEN_WIN_HAS_ACTIVE
                | WAYWALLEN_WIN_HAS_MAXIMIZED
        );
        assert_eq!(
            WindowState::from_event(&payload(&[State::Activated, State::Fullscreen])).to_flags(),
            WAYWALLEN_WIN_HAS_NON_MINIMIZED
                | WAYWALLEN_WIN_HAS_ACTIVE
                | WAYWALLEN_WIN_HAS_FULLSCREEN
        );
    }

    #[test]
    fn minimized_windows_contribute_nothing() {
        assert_eq!(
            WindowState::from_event(&payload(&[State::Minimized, State::Activated])).to_flags(),
            0
        );
        assert_eq!(
            WindowState::from_event(&payload(&[State::Minimized])).to_flags(),
            0
        );
    }

    #[test]
    fn a_truncated_array_is_ignored() {
        assert_eq!(
            WindowState::from_event(&[0x02, 0x00, 0x00]),
            WindowState::default()
        );
    }
}
