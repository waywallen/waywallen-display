use std::path::{Path};
use std::thread;
use std::collections::HashMap;
use std::sync::{Arc, MutexGuard, PoisonError};
use std::sync::atomic::Ordering;
use niri_ipc::{Event, Request, Response, Window};
use niri_ipc::socket::Socket;
use niri_ipc::state::{EventStreamState, EventStreamStatePart, WindowsState, WorkspacesState};
use thiserror::Error;
use waywallen_display::{WAYWALLEN_WIN_HAS_ACTIVE, WAYWALLEN_WIN_HAS_FULLSCREEN, WAYWALLEN_WIN_HAS_NON_MINIMIZED};
use crate::OutputBinding;
use crate::watcher::{handle_return_code, BindingRegistry};

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

pub fn spawn(registry: BindingRegistry) {
    let Some(sock) = detect_socket() else {
        return;
    };
    log::info!("niri_watcher: enabled (socket={})", sock.as_ref().display());
    Socket::connect_to(sock.as_ref()).map(|mut event_socket|
        event_socket.send(Request::EventStream).map(|reply| match reply {
            Ok(response) => match response {
                Response::Handled => {
                    thread::spawn(move || run_loop(event_socket, registry));
                },
                response => log::error!("niri_watcher: {}", Error::UnexpectedResponse(response))
            }
            Err(error) => log::error!("niri_watcher: {}", Error::CompositorResponse(error))
        }).unwrap_or_else(|error| log::error!("niri_watcher: request eventstream: {error}"))
    ).unwrap_or_else(|error| log::error!("niri_watcher: connect {}: {error}", sock.as_ref().display()))
}

fn run_loop(event_socket: Socket, registry: BindingRegistry) {
    let mut state = EventStreamState::default();
    let mut read_event = event_socket.read_events();
    loop {
        read_event().map(|event| {
            log::debug!("niri_watcher: niri event: {:?}", event);
            if matches!(event, Event::WindowLayoutsChanged {..} | Event::WindowOpenedOrChanged { .. } | Event::WindowFocusChanged {..} | Event::WorkspaceActivated {..}) {
                state.apply(event);
                log::debug!("niri_watcher: layouts changed, refreshing outputs");
                registry.lock().map(|registry| {
                    get_outputs_flags(&*registry, &state.workspaces, &state.windows).for_each(|flags| {
                        flags.map(|(output, flags)| {
                            output.with_display(|display| unsafe {
                                waywallen_display::waywallen_display_set_window_state(display, flags)
                            }).map_or((), |return_code| {
                                handle_return_code("niri_watcher", return_code, flags, output);
                            })
                        }).unwrap_or_else(|error| log::error!("niri_watcher: calculate flag: {error}"))
                    })
                }).unwrap_or_else(|error| log::error!("niri_watcher: lock registry: {error}"))
            } else {
                state.apply(event);
            }
        }).unwrap_or_else(|error| log::error!("niri_watcher: read event: {error}"));
    }
}

fn get_outputs_flags<'a>(
    outputs: &'a HashMap<String, Arc<OutputBinding>>,
    workspaces_state: &'a WorkspacesState,
    windows_state: &'a WindowsState
) -> impl Iterator<Item = Result<(&'a Arc<OutputBinding>, u32), PoisonError<MutexGuard<'a, Option<(u32, u32)>>>>> {
    workspaces_state.workspaces.values()
        .filter_map(|workspace| {
            workspace.is_active.then_some(())?;
            workspace.output.as_ref().map(|output|
                outputs.get(output).map(|output| {
                    output.is_registered().then_some(())?;
                    workspace.active_window_id.map(|active_window_id|
                        windows_state.windows.get(&active_window_id).map(|active_window|
                            match output.logical_size.lock() {
                                Ok(logical_size) => logical_size.map(|(x, y)| {
                                    let flags = window_to_flags((x as i32, y as i32), active_window);
                                    log::debug!("niri_watcher: {} flags: {flags}", output.display_name());
                                    let old_flags = output.window_flags().swap(0, Ordering::SeqCst);
                                    (old_flags != 0).then_some(Ok((output, 0)))
                                }).flatten(),
                                Err(error) => Some(Err(error))
                            }
                        ).flatten()
                    ).unwrap_or_else(|| {
                        log::debug!("niri_watcher: {} flags: 0", output.display_name());
                        let old_flags = output.window_flags().swap(0, Ordering::SeqCst);
                        (old_flags != 0).then_some(Ok((output, 0)))
                    })
                }).flatten()
            ).flatten()
        })
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