//! Window-state watcher for COSMIC (cosmic-comp) over its own protocols.
//!
//! cosmic-comp ships neither the compositors' IPCs nor
//! `zwlr_foreign_toplevel_management_v1` — it exposes the upstream
//! `ext_foreign_toplevel_list_v1` plus two extensions instead:
//!
//! * `zcosmic_toplevel_info_v1` extends every toplevel with the
//!   `maximized` / `minimized` / `activated` / `fullscreen` / `sticky`
//!   state bits, `output_enter` / `output_leave`, and (since v3)
//!   `ext_workspace_enter` / `ext_workspace_leave`.
//! * `ext_workspace_manager_v1` announces every workspace and which of
//!   them are currently active — exactly the piece the wlr fallback
//!   fundamentally cannot know.
//!
//! Combining the two closes the gap that limits [`super::wlr`]: COSMIC
//! keeps a window's `output_enter` set while its desktop is hidden, so
//! the workspace events are what let windows parked on an inactive
//! desktop drop out of the count instead of keeping `HAS_NON_MINIMIZED`
//! et al. lit. Toplevels without workspace membership (sticky windows)
//! or a missing workspace manager fall back to output-only accounting
//! and err toward "visible".
//!
//! Upstream both globals sit behind cosmic-comp's `client_not_sandboxed`
//! filter, i.e. they are open to every regular session client. The
//! `ext_workspace_*` handles referenced by a toplevel are the very same
//! resources the manager announced to us, because both globals were
//! bound on this one connection — matching them by object id is sound.

use super::cosmic_toplevel_info::{
    zcosmic_toplevel_handle_v1::{self, State as ToplevelState, ZcosmicToplevelHandleV1},
    zcosmic_toplevel_info_v1::{self, ZcosmicToplevelInfoV1},
};
use crate::watcher::{Command, CommandSender};
use std::collections::{HashMap, HashSet};
use std::thread;
use wayland_client::backend::ObjectId;
use wayland_client::globals::{registry_queue_init, GlobalListContents};
use wayland_client::protocol::wl_output::{self, WlOutput};
use wayland_client::protocol::wl_registry::{self, WlRegistry};
use wayland_client::{
    event_created_child, Connection, Dispatch, EventQueue, Proxy, QueueHandle,
};
use wayland_protocols::ext::foreign_toplevel_list::v1::client::{
    ext_foreign_toplevel_handle_v1::{self, ExtForeignToplevelHandleV1},
    ext_foreign_toplevel_list_v1::{self, ExtForeignToplevelListV1},
};
use wayland_protocols::ext::workspace::v1::client::{
    ext_workspace_group_handle_v1::{self, ExtWorkspaceGroupHandleV1},
    ext_workspace_handle_v1::{self, ExtWorkspaceHandleV1},
    ext_workspace_manager_v1::{self, ExtWorkspaceManagerV1},
};
use wayland_client::WEnum;
use waywallen_display::{
    WAYWALLEN_WIN_HAS_ACTIVE, WAYWALLEN_WIN_HAS_FULLSCREEN, WAYWALLEN_WIN_HAS_MAXIMIZED,
    WAYWALLEN_WIN_HAS_NON_MINIMIZED,
};

const FOREIGN_TOPLEVEL_LIST_INTERFACE: &str = "ext_foreign_toplevel_list_v1";
const TOPLEVEL_INFO_INTERFACE: &str = "zcosmic_toplevel_info_v1";
const WORKSPACE_MANAGER_INTERFACE: &str = "ext_workspace_manager_v1";

/// Version 2 added `get_cosmic_toplevel`; everything before is legacy.
const TOPLEVEL_INFO_VERSION: u32 = 3;
/// The only version there is.
const FOREIGN_TOPLEVEL_LIST_VERSION: u32 = 1;
const WORKSPACE_MANAGER_VERSION: u32 = 1;

/// `wl_output.name` carries the connector string the daemon knows each
/// display by, and it needs version 4.
const OUTPUT_VERSION: u32 = 4;

pub fn detect() -> bool {
    let Ok(connection) = Connection::connect_to_env() else {
        return false;
    };
    let Ok((globals, _queue)) = registry_queue_init::<Probe>(&connection) else {
        return false;
    };
    let mut foreign_toplevel_list = false;
    let mut toplevel_info = false;
    for global in globals.contents().clone_list() {
        match global.interface.as_str() {
            FOREIGN_TOPLEVEL_LIST_INTERFACE => foreign_toplevel_list = true,
            TOPLEVEL_INFO_INTERFACE => toplevel_info = global.version >= 2,
            _ => {}
        }
    }
    foreign_toplevel_list && toplevel_info
}

struct Probe;

impl Dispatch<WlRegistry, GlobalListContents> for Probe {
    fn event(
        _: &mut Self,
        _: &WlRegistry,
        _: wl_registry::Event,
        _: &GlobalListContents,
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}

pub fn spawn(commands: CommandSender) {
    thread::spawn(move || run_loop(commands));
}

fn run_loop(commands: CommandSender) {
    let connection = match Connection::connect_to_env() {
        Ok(connection) => connection,
        Err(error) => {
            log::error!("cosmic_watcher: connect to compositor: {error}");
            return;
        }
    };
    let (globals, queue) = match registry_queue_init::<Watcher>(&connection) {
        Ok(pair) => pair,
        Err(error) => {
            log::error!("cosmic_watcher: registry init: {error}");
            return;
        }
    };
    let qh = queue.handle();
    let mut watcher = Watcher::new(commands);
    let mut foreign_toplevel_list = None;
    let mut toplevel_info = None;
    let mut workspace_manager = None;
    for global in globals.contents().clone_list() {
        match global.interface.as_str() {
            FOREIGN_TOPLEVEL_LIST_INTERFACE => {
                foreign_toplevel_list = Some(
                    globals
                        .registry()
                        .bind::<ExtForeignToplevelListV1, _, _>(
                            global.name,
                            global.version.min(FOREIGN_TOPLEVEL_LIST_VERSION),
                            &qh,
                            (),
                        ),
                );
            }
            TOPLEVEL_INFO_INTERFACE if global.version >= 2 => {
                toplevel_info = Some(
                    globals
                        .registry()
                        .bind::<ZcosmicToplevelInfoV1, _, _>(
                            global.name,
                            global.version.min(TOPLEVEL_INFO_VERSION),
                            &qh,
                            (),
                        ),
                );
            }
            WORKSPACE_MANAGER_INTERFACE => {
                // Optional: without it every toplevel counts as visible,
                // which degrades to the wlr fallback's blind spots.
                workspace_manager = Some(
                    globals
                        .registry()
                        .bind::<ExtWorkspaceManagerV1, _, _>(
                            global.name,
                            global.version.min(WORKSPACE_MANAGER_VERSION),
                            &qh,
                            (),
                        ),
                );
            }
            "wl_output" => {
                watcher.bind_output(globals.registry(), global.name, global.version, &qh)
            }
            _ => {}
        }
    }
    let (Some(_foreign_toplevel_list), Some(toplevel_info)) =
        (foreign_toplevel_list, toplevel_info)
    else {
        log::error!(
            "cosmic_watcher: compositor does not expose {FOREIGN_TOPLEVEL_LIST_INTERFACE} \
             with {TOPLEVEL_INFO_INTERFACE} v2+"
        );
        return;
    };
    if workspace_manager.is_none() {
        log::debug!(
            "cosmic_watcher: no {WORKSPACE_MANAGER_INTERFACE}, \
             hidden-desktop filtering disabled"
        );
    }
    log::info!(
        "cosmic_watcher: enabled ({TOPLEVEL_INFO_INTERFACE} v{}, \
         {WORKSPACE_MANAGER_INTERFACE} v{})",
        toplevel_info.version(),
        workspace_manager
            .as_ref()
            .map(Proxy::version)
            .unwrap_or(0)
    );
    watcher.set_managers(toplevel_info);
    run_dispatch(queue, watcher);
}

fn run_dispatch(mut queue: EventQueue<Watcher>, mut watcher: Watcher) {
    // Binding the toplevel list makes the compositor announce every window
    // it already has, so the first roundtrip is what fills in the state
    // the daemon starts out with.
    if let Err(error) = queue.roundtrip(&mut watcher) {
        log::error!("cosmic_watcher: initial roundtrip: {error}");
        return;
    }
    watcher.push_state();
    loop {
        if let Err(error) = queue.blocking_dispatch(&mut watcher) {
            log::error!("cosmic_watcher: dispatch: {error}");
            return;
        }
        if std::mem::take(&mut watcher.dirty) {
            watcher.push_state();
        }
    }
}

struct Output {
    global: u32,
    display_name: Option<String>,
}

#[derive(Default)]
struct Toplevel {
    state: WindowState,
    outputs: HashSet<ObjectId>,
    workspaces: HashSet<ObjectId>,
}

#[derive(Default)]
struct Workspace {
    active: bool,
}

struct Watcher {
    commands: CommandSender,
    outputs: HashMap<ObjectId, Output>,
    /// Keyed by the `ext_foreign_toplevel_handle_v1` id.
    toplevels: HashMap<ObjectId, Toplevel>,
    /// `zcosmic_toplevel_handle_v1` extension objects remember which
    /// ext handle they belong to.
    cosmic_handles: HashMap<ObjectId, ObjectId>,
    workspaces: HashMap<ObjectId, Workspace>,
    toplevel_info: Option<ZcosmicToplevelInfoV1>,
    /// Turns true with the first announced workspace; until then the
    /// accounting cannot filter anything.
    tracking_workspaces: bool,
    dirty: bool,
}

#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
struct WindowState {
    maximized: bool,
    minimized: bool,
    activated: bool,
    fullscreen: bool,
    sticky: bool,
}

impl WindowState {
    /// The payload is a `wl_array` of `zcosmic_toplevel_handle_v1.state`
    /// values and every event replaces the previous state wholesale.
    fn from_event(payload: &[u8]) -> Self {
        let mut state = Self::default();
        for value in payload.chunks_exact(4) {
            let value = u32::from_ne_bytes([value[0], value[1], value[2], value[3]]);
            match ToplevelState::try_from(value) {
                Ok(ToplevelState::Maximized) => state.maximized = true,
                Ok(ToplevelState::Minimized) => state.minimized = true,
                Ok(ToplevelState::Activated) => state.activated = true,
                Ok(ToplevelState::Fullscreen) => state.fullscreen = true,
                Ok(ToplevelState::Sticky) => state.sticky = true,
                Err(_) => log::debug!("cosmic_watcher: unknown toplevel state {value}"),
            }
        }
        state
    }

    /// A minimized window is off screen, so it contributes nothing.
    /// Sticky has no bit of its own; it only matters for [`Watcher`].
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
    fn new(commands: CommandSender) -> Self {
        Self {
            commands,
            outputs: HashMap::new(),
            toplevels: HashMap::new(),
            cosmic_handles: HashMap::new(),
            workspaces: HashMap::new(),
            toplevel_info: None,
            tracking_workspaces: false,
            dirty: false,
        }
    }

    /// Keeps the toplevel-info proxy around: new windows need it to ask
    /// for their `zcosmic_toplevel_handle_v1` extension object.
    fn set_managers(&mut self, toplevel_info: ZcosmicToplevelInfoV1) {
        self.toplevel_info = Some(toplevel_info);
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
                "cosmic_watcher: wl_output v{version} has no name event, \
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

    /// A window counts toward a display while at least one of its
    /// workspaces is showing on it. Without workspace knowledge there is
    /// nothing to go on, and sticky windows show up everywhere anyway.
    fn is_visible(&self, toplevel: &Toplevel) -> bool {
        toplevel.state.sticky
            || !self.tracking_workspaces
            || toplevel.workspaces.is_empty()
            || toplevel
                .workspaces
                .iter()
                .any(|id| self.workspaces.get(id).is_some_and(|ws| ws.active))
    }

    fn aggregate_flags(&self) -> HashMap<String, u32> {
        let mut by_output: HashMap<String, u32> = HashMap::new();
        for toplevel in self.toplevels.values() {
            let flags = toplevel.state.to_flags();
            if flags == 0 || !self.is_visible(toplevel) {
                continue;
            }
            for output in &toplevel.outputs {
                let Some(display_name) = self
                    .outputs
                    .get(output)
                    .and_then(|output| output.display_name.clone())
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
        for display_name in self
            .outputs
            .values()
            .filter_map(|output| output.display_name.as_deref())
        {
            let flags = by_output.get(display_name).copied().unwrap_or(0);
            log::debug!("cosmic_watcher: {display_name} flags: {flags}");
            self.commands.send(Command::WindowState {
                display_name: display_name.to_string(),
                flags,
            });
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
        log::debug!("cosmic_watcher: wl_output {} is '{name}'", entry.global);
        entry.display_name = Some(name);
        state.dirty = true;
    }
}

impl Dispatch<ExtForeignToplevelListV1, ()> for Watcher {
    fn event(
        state: &mut Self,
        _: &ExtForeignToplevelListV1,
        event: ext_foreign_toplevel_list_v1::Event,
        _: &(),
        _: &Connection,
        qh: &QueueHandle<Self>,
    ) {
        match event {
            ext_foreign_toplevel_list_v1::Event::Toplevel { toplevel } => {
                let ext_id = toplevel.id();
                state.toplevels.insert(ext_id.clone(), Toplevel::default());
                // Ask for the extension object right away; the initial
                // state batch arrives on it before the next done.
                if let Some(info) = state.toplevel_info.as_ref() {
                    let cosmic_handle = info.get_cosmic_toplevel(&toplevel, qh, ());
                    state.cosmic_handles.insert(cosmic_handle.id(), ext_id);
                }
            }
            ext_foreign_toplevel_list_v1::Event::Finished => {
                log::info!("cosmic_watcher: compositor stopped the toplevel list");
                state.toplevels.clear();
                state.cosmic_handles.clear();
                state.dirty = true;
            }
            _ => {}
        }
    }

    event_created_child!(Watcher, ExtForeignToplevelListV1, [
        ext_foreign_toplevel_list_v1::EVT_TOPLEVEL_OPCODE => (ExtForeignToplevelHandleV1, ()),
    ]);
}

impl Dispatch<ExtForeignToplevelHandleV1, ()> for Watcher {
    fn event(
        state: &mut Self,
        handle: &ExtForeignToplevelHandleV1,
        event: ext_foreign_toplevel_handle_v1::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        // title / app_id / identifier / done carry nothing we need.
        let ext_foreign_toplevel_handle_v1::Event::Closed = event else {
            return;
        };
        handle.destroy();
        state.toplevels.remove(&handle.id());
        state.cosmic_handles.retain(|_, ext| *ext != handle.id());
        state.dirty = true;
    }
}

impl Dispatch<ZcosmicToplevelInfoV1, ()> for Watcher {
    fn event(
        state: &mut Self,
        _: &ZcosmicToplevelInfoV1,
        event: zcosmic_toplevel_info_v1::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        // The manager-wide done is the commit point of a refresh cycle.
        if let zcosmic_toplevel_info_v1::Event::Done = event {
            state.dirty = true;
        }
    }
}

impl Dispatch<ZcosmicToplevelHandleV1, ()> for Watcher {
    fn event(
        state: &mut Self,
        handle: &ZcosmicToplevelHandleV1,
        event: zcosmic_toplevel_handle_v1::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        // title / app_id / geometry are irrelevant here, and done/closed
        // only exist for v1 clients — removal arrives via the ext handle.
        let Some(ext_id) = state.cosmic_handles.get(&handle.id()).cloned() else {
            return;
        };
        match event {
            zcosmic_toplevel_handle_v1::Event::State { state: reported } => {
                let Some(toplevel) = state.toplevels.get_mut(&ext_id) else {
                    return;
                };
                let new_state = WindowState::from_event(&reported);
                if toplevel.state != new_state {
                    log::debug!(
                        "cosmic_watcher: toplevel {ext_id} state {:?}",
                        toplevel.state
                    );
                    toplevel.state = new_state;
                    state.dirty = true;
                }
            }
            zcosmic_toplevel_handle_v1::Event::OutputEnter { output } => {
                if let Some(toplevel) = state.toplevels.get_mut(&ext_id) {
                    toplevel.outputs.insert(output.id());
                    state.dirty = true;
                }
            }
            zcosmic_toplevel_handle_v1::Event::OutputLeave { output } => {
                if let Some(toplevel) = state.toplevels.get_mut(&ext_id) {
                    toplevel.outputs.remove(&output.id());
                    state.dirty = true;
                }
            }
            zcosmic_toplevel_handle_v1::Event::ExtWorkspaceEnter { workspace } => {
                if let Some(toplevel) = state.toplevels.get_mut(&ext_id) {
                    toplevel.workspaces.insert(workspace.id());
                    state.dirty = true;
                }
            }
            zcosmic_toplevel_handle_v1::Event::ExtWorkspaceLeave { workspace } => {
                if let Some(toplevel) = state.toplevels.get_mut(&ext_id) {
                    toplevel.workspaces.remove(&workspace.id());
                    state.dirty = true;
                }
            }
            _ => {}
        }
    }
}

impl Dispatch<ExtWorkspaceManagerV1, ()> for Watcher {
    fn event(
        state: &mut Self,
        _: &ExtWorkspaceManagerV1,
        event: ext_workspace_manager_v1::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        match event {
            ext_workspace_manager_v1::Event::Workspace { workspace, .. } => {
                state.workspaces.insert(workspace.id(), Workspace::default());
                state.tracking_workspaces = true;
                state.dirty = true;
            }
            ext_workspace_manager_v1::Event::Done => state.dirty = true,
            ext_workspace_manager_v1::Event::Finished => {
                log::info!("cosmic_watcher: compositor stopped the workspace manager");
                state.workspaces.clear();
                state.tracking_workspaces = false;
                state.dirty = true;
            }
            _ => {}
        }
    }

    event_created_child!(Watcher, ExtWorkspaceManagerV1, [
        ext_workspace_manager_v1::EVT_WORKSPACE_GROUP_OPCODE => (ExtWorkspaceGroupHandleV1, ()),
        ext_workspace_manager_v1::EVT_WORKSPACE_OPCODE => (ExtWorkspaceHandleV1, ()),
    ]);
}

impl Dispatch<ExtWorkspaceGroupHandleV1, ()> for Watcher {
    fn event(
        _: &mut Self,
        group: &ExtWorkspaceGroupHandleV1,
        event: ext_workspace_group_handle_v1::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        // Groups only relate workspaces to outputs; per-toplevel output
        // enter/leave already covers that mapping.
        if let ext_workspace_group_handle_v1::Event::Removed = event {
            group.destroy();
        }
    }
}

impl Dispatch<ExtWorkspaceHandleV1, ()> for Watcher {
    fn event(
        state: &mut Self,
        handle: &ExtWorkspaceHandleV1,
        event: ext_workspace_handle_v1::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        match event {
            ext_workspace_handle_v1::Event::State { state: reported } => {
                // A bitmask, not a list: missing bits convey the opposite
                // meaning, and combined bits decode as Unknown — so take
                // the raw value and test the active bit.
                let raw = match reported {
                    WEnum::Value(value) => u32::from(value),
                    WEnum::Unknown(raw) => raw,
                };
                let Some(workspace) = state.workspaces.get_mut(&handle.id()) else {
                    return;
                };
                let active =
                    raw & u32::from(ext_workspace_handle_v1::State::Active) != 0;
                if workspace.active != active {
                    workspace.active = active;
                    state.dirty = true;
                }
            }
            ext_workspace_handle_v1::Event::Removed => {
                handle.destroy();
                state.workspaces.remove(&handle.id());
                state.dirty = true;
            }
            _ => {}
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::watcher::command_channel;

    fn payload(states: &[ToplevelState]) -> Vec<u8> {
        states
            .iter()
            .flat_map(|state| u32::from(*state).to_ne_bytes())
            .collect()
    }

    fn watcher() -> Watcher {
        let (sender, _receiver) = command_channel().unwrap();
        Watcher::new(sender)
    }

    /// Null object ids are all we need: the maps only ever compare keys
    /// against each other, and every test uses at most one id per kind.
    fn null_id() -> ObjectId {
        ObjectId::null()
    }

    fn output_named(name: &str) -> (ObjectId, Output) {
        (
            null_id(),
            Output {
                global: 1,
                display_name: Some(name.to_string()),
            },
        )
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
            WindowState::from_event(&payload(&[ToplevelState::Activated])).to_flags(),
            WAYWALLEN_WIN_HAS_NON_MINIMIZED | WAYWALLEN_WIN_HAS_ACTIVE
        );
        assert_eq!(
            WindowState::from_event(&payload(&[ToplevelState::Maximized, ToplevelState::Activated]))
                .to_flags(),
            WAYWALLEN_WIN_HAS_NON_MINIMIZED
                | WAYWALLEN_WIN_HAS_ACTIVE
                | WAYWALLEN_WIN_HAS_MAXIMIZED
        );
        assert_eq!(
            WindowState::from_event(&payload(&[
                ToplevelState::Activated,
                ToplevelState::Fullscreen
            ]))
            .to_flags(),
            WAYWALLEN_WIN_HAS_NON_MINIMIZED | WAYWALLEN_WIN_HAS_ACTIVE | WAYWALLEN_WIN_HAS_FULLSCREEN
        );
        // Sticky is tracked but has no flag of its own.
        let sticky = WindowState::from_event(&payload(&[ToplevelState::Sticky]));
        assert!(sticky.sticky);
        assert_eq!(sticky.to_flags(), WAYWALLEN_WIN_HAS_NON_MINIMIZED);
    }

    #[test]
    fn minimized_windows_contribute_nothing() {
        assert_eq!(
            WindowState::from_event(&payload(&[ToplevelState::Minimized, ToplevelState::Activated]))
                .to_flags(),
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

    #[test]
    fn flags_are_attributed_per_display() {
        let mut w = watcher();
        let (output_id, output) = output_named("DP-1");
        w.outputs.insert(output_id.clone(), output);
        w.toplevels.insert(
            null_id(),
            Toplevel {
                state: WindowState {
                    activated: true,
                    ..Default::default()
                },
                outputs: [output_id].into(),
                ..Default::default()
            },
        );
        assert_eq!(
            w.aggregate_flags(),
            HashMap::from([(
                "DP-1".to_string(),
                WAYWALLEN_WIN_HAS_NON_MINIMIZED | WAYWALLEN_WIN_HAS_ACTIVE
            )])
        );
    }

    #[test]
    fn a_window_on_an_inactive_workspace_is_skipped() {
        let mut w = watcher();
        let (output_id, output) = output_named("DP-1");
        w.outputs.insert(output_id.clone(), output);
        let workspace_id = null_id();
        w.tracking_workspaces = true;
        w.workspaces.insert(workspace_id.clone(), Workspace { active: false });
        w.toplevels.insert(
            null_id(),
            Toplevel {
                state: WindowState {
                    maximized: true,
                    activated: true,
                    ..Default::default()
                },
                outputs: [output_id].into(),
                workspaces: [workspace_id].into(),
            },
        );
        assert!(w.aggregate_flags().is_empty());
    }

    #[test]
    fn a_window_on_the_active_workspace_counts() {
        let mut w = watcher();
        let (output_id, output) = output_named("DP-1");
        w.outputs.insert(output_id.clone(), output);
        let workspace_id = null_id();
        w.tracking_workspaces = true;
        w.workspaces.insert(workspace_id.clone(), Workspace { active: true });
        w.toplevels.insert(
            null_id(),
            Toplevel {
                state: WindowState {
                    fullscreen: true,
                    activated: true,
                    ..Default::default()
                },
                outputs: [output_id].into(),
                workspaces: [workspace_id].into(),
            },
        );
        assert_eq!(
            w.aggregate_flags()["DP-1"],
            WAYWALLEN_WIN_HAS_NON_MINIMIZED
                | WAYWALLEN_WIN_HAS_ACTIVE
                | WAYWALLEN_WIN_HAS_FULLSCREEN
        );
    }

    #[test]
    fn sticky_windows_are_always_visible() {
        let mut w = watcher();
        let (output_id, output) = output_named("DP-1");
        w.outputs.insert(output_id.clone(), output);
        let workspace_id = null_id();
        w.tracking_workspaces = true;
        w.workspaces.insert(workspace_id.clone(), Workspace { active: false });
        w.toplevels.insert(
            null_id(),
            Toplevel {
                state: WindowState {
                    sticky: true,
                    maximized: true,
                    ..Default::default()
                },
                outputs: [output_id].into(),
                workspaces: [workspace_id].into(),
            },
        );
        assert_eq!(
            w.aggregate_flags()["DP-1"],
            WAYWALLEN_WIN_HAS_NON_MINIMIZED | WAYWALLEN_WIN_HAS_MAXIMIZED
        );
    }

    #[test]
    fn without_workspace_accounting_everything_counts() {
        let mut w = watcher();
        let (output_id, output) = output_named("DP-1");
        w.outputs.insert(output_id.clone(), output);
        w.toplevels.insert(
            null_id(),
            Toplevel {
                state: WindowState {
                    fullscreen: true,
                    ..Default::default()
                },
                outputs: [output_id].into(),
                workspaces: [null_id()].into(),
            },
        );
        assert_eq!(
            w.aggregate_flags()["DP-1"],
            WAYWALLEN_WIN_HAS_NON_MINIMIZED | WAYWALLEN_WIN_HAS_FULLSCREEN
        );
    }
}


