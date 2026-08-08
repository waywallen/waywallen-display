use crate::watcher::{BindingRegistry, Command, CommandSender, OutputInfo};
use niri_ipc::socket::Socket;
use niri_ipc::state::{EventStreamState, EventStreamStatePart, WindowsState, WorkspacesState};
use niri_ipc::{Event, Request, Response, Window};
use std::collections::HashMap;
use std::path::Path;
use std::sync::Arc;
use std::thread;
use thiserror::Error;
use waywallen_display::{
    WAYWALLEN_WIN_HAS_ACTIVE, WAYWALLEN_WIN_HAS_FULLSCREEN, WAYWALLEN_WIN_HAS_NON_MINIMIZED,
};

#[derive(Error, Debug)]
pub enum Error {
    #[error("compositor response: {0}")]
    CompositorResponse(String),
    #[error("unexpected response: {0:?}")]
    UnexpectedResponse(Response),
}

pub fn detect_socket() -> Option<impl AsRef<Path>> {
    let niri_socket = std::env::var_os("NIRI_SOCKET")?;
    let path: &Path = niri_socket.as_ref();
    if path.exists() {
        Some(niri_socket)
    } else {
        None
    }
}

pub fn spawn(registry: BindingRegistry, commands: CommandSender) {
    let Some(sock) = detect_socket() else {
        return;
    };
    log::info!("niri_watcher: enabled (socket={})", sock.as_ref().display());
    Socket::connect_to(sock.as_ref())
        .map(|mut event_socket| {
            event_socket
                .send(Request::EventStream)
                .map(|reply| match reply {
                    Ok(response) => match response {
                        Response::Handled => {
                            thread::spawn(move || run_loop(event_socket, registry, commands));
                        }
                        response => {
                            log::error!("niri_watcher: {}", Error::UnexpectedResponse(response))
                        }
                    },
                    Err(error) => log::error!("niri_watcher: {}", Error::CompositorResponse(error)),
                })
                .unwrap_or_else(|error| log::error!("niri_watcher: request eventstream: {error}"))
        })
        .unwrap_or_else(|error| {
            log::error!("niri_watcher: connect {}: {error}", sock.as_ref().display())
        })
}

fn run_loop(event_socket: Socket, registry: BindingRegistry, commands: CommandSender) {
    let mut state = EventStreamState::default();
    let mut read_event = event_socket.read_events();
    loop {
        read_event()
            .map(|event| {
                log::debug!("niri_watcher: niri event: {:?}", event);
                if matches!(
                    event,
                    Event::WindowLayoutsChanged { .. }
                        | Event::WindowOpenedOrChanged { .. }
                        | Event::WindowFocusChanged { .. }
                        | Event::WorkspaceActivated { .. }
                ) {
                    state.apply(event);
                    registry
                        .lock()
                        .map(|registry| {
                            get_outputs_flags(&*registry, &state.workspaces, &state.windows)
                                .into_iter()
                                .for_each(|(output, flags)| {
                                    commands.send(Command::WindowState {
                                        display_name: output.display_name().to_string(),
                                        flags,
                                    });
                                })
                        })
                        .unwrap_or_else(|error| log::error!("niri_watcher: lock registry: {error}"))
                } else {
                    state.apply(event);
                }
            })
            .unwrap_or_else(|error| log::error!("niri_watcher: read event: {error}"));
    }
}

fn get_outputs_flags(
    outputs: &HashMap<String, Arc<OutputInfo>>,
    workspaces_state: &WorkspacesState,
    windows_state: &WindowsState,
) -> Vec<(Arc<OutputInfo>, u32)> {
    let mut changed = Vec::new();
    for workspace in workspaces_state
        .workspaces
        .values()
        .filter(|workspace| workspace.is_active)
    {
        let Some(output_name) = workspace.output.as_ref() else {
            continue;
        };
        let Some(output) = outputs.get(output_name) else {
            continue;
        };
        let flags = workspace
            .active_window_id
            .and_then(|id| windows_state.windows.get(&id))
            .and_then(|window| {
                output
                    .logical_size()
                    .map(|(width, height)| window_to_flags((width as i32, height as i32), window))
            })
            .unwrap_or(0);
        log::debug!("niri_watcher: {} flags: {flags}", output.display_name());
        if output.replace_window_flags(flags) {
            changed.push((output.clone(), flags));
        }
    }
    changed
}

// Implementation note: niri can never have an unfocused window - it doesn't support minimization
// Also, for now, the IPC doesn't report fullscreen / maximize state, so we have to guess for
// fullscreen and just do without maximization.
fn window_to_flags(fullscreen: (i32, i32), window: &Window) -> u32 {
    let mut flags = 0;
    flags |= WAYWALLEN_WIN_HAS_NON_MINIMIZED;
    flags |= WAYWALLEN_WIN_HAS_ACTIVE;
    if is_window_fullscreen(fullscreen, window) {
        flags |= WAYWALLEN_WIN_HAS_FULLSCREEN
    } /* else if is_window_maximized(fullscreen, window) {
          flags |= WAYWALLEN_WIN_HAS_MAXIMIZED
      } */
    flags
}

// TODO: Waiting on https://github.com/niri-wm/niri/pull/2836 for proper logic
fn is_window_fullscreen(fullscreen: (i32, i32), window: &Window) -> bool {
    !window.is_floating && window.layout.window_size == fullscreen
}
