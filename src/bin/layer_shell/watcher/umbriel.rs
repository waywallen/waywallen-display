//! Window-state watcher for Umbriel over its `$UMBRIEL_SOCKET` JSON-line IPC.

use crate::watcher::{BindingRegistry, Command, CommandSender, Window};
use serde::Deserialize;
use std::collections::{HashMap, HashSet};
use std::io::{BufRead, BufReader, Write};
use std::os::unix::net::UnixStream;
use std::path::{Path, PathBuf};
use std::thread;
use std::time::Duration;
use waywallen_display::{
    WAYWALLEN_WIN_HAS_ACTIVE, WAYWALLEN_WIN_HAS_FULLSCREEN, WAYWALLEN_WIN_HAS_MAXIMIZED,
    WAYWALLEN_WIN_HAS_NON_MINIMIZED,
};

const SUBSCRIBE_CMD: &[u8] = b"{\"cmd\":\"subscribe\",\"events\":[\"windows\",\"workspaces\"]}\n";
const RECONNECT_DELAY: Duration = Duration::from_secs(2);

pub fn detect_socket() -> Option<PathBuf> {
    let umbriel_socket = std::env::var_os("UMBRIEL_SOCKET")?;
    if umbriel_socket.is_empty() {
        return None;
    }
    let path = PathBuf::from(umbriel_socket);
    path.exists().then_some(path)
}

pub fn spawn(registry: BindingRegistry, commands: CommandSender) -> bool {
    let Some(sock) = detect_socket() else {
        return false;
    };
    log::info!("umbriel_watcher: enabled (socket={})", sock.display());
    let mut stream = match UnixStream::connect(&sock) {
        Ok(stream) => stream,
        Err(error) => {
            log::warn!("umbriel_watcher: connect {}: {error}", sock.display());
            return false;
        }
    };
    if let Err(error) = send_subscribe(&mut stream) {
        log::warn!("umbriel_watcher: subscribe {}: {error}", sock.display());
        return false;
    }
    thread::spawn(move || run_loop(sock, stream, registry, commands));
    true
}

fn send_subscribe(stream: &mut impl Write) -> std::io::Result<()> {
    stream.write_all(SUBSCRIBE_CMD)?;
    stream.flush()
}

fn run_loop(
    socket_path: PathBuf,
    initial_stream: UnixStream,
    registry: BindingRegistry,
    commands: CommandSender,
) {
    let mut initial_stream = Some(initial_stream);
    loop {
        let stream = match initial_stream.take() {
            Some(stream) => Ok(stream),
            None => {
                if !Path::new(&socket_path).exists() {
                    log::info!(
                        "umbriel_watcher: socket {} gone, stopping",
                        socket_path.display()
                    );
                    break;
                }
                UnixStream::connect(&socket_path).and_then(|mut s| {
                    send_subscribe(&mut s)?;
                    Ok(s)
                })
            }
        };

        match stream {
            Ok(stream) => read_stream(stream, &registry, &commands),
            Err(error) => {
                log::warn!(
                    "umbriel_watcher: connect {}: {error}",
                    socket_path.display()
                );
            }
        }
        thread::sleep(RECONNECT_DELAY);
    }
}

fn read_stream(stream: UnixStream, registry: &BindingRegistry, commands: &CommandSender) {
    let reader = BufReader::new(stream);
    let mut state = StreamState::default();
    for line in reader.lines() {
        let line = match line {
            Ok(line) => line,
            Err(error) => {
                log::warn!("umbriel_watcher: read event: {error}");
                break;
            }
        };
        if line.trim().is_empty() {
            continue;
        }
        match serde_json::from_str::<StreamMessage>(&line) {
            Ok(StreamMessage::Event(event)) => {
                state.apply(event);
                if let (Some(workspaces), Some(windows)) = (&state.workspaces, &state.windows) {
                    let snapshots = {
                        let outputs = registry.lock().unwrap();
                        collect_outputs_windows(
                            outputs.keys().map(String::as_str),
                            workspaces,
                            windows,
                        )
                    };
                    for (display_name, windows) in snapshots {
                        commands.send(Command::WindowState {
                            display_name,
                            windows,
                        });
                    }
                }
            }
            Ok(StreamMessage::Error { err }) => {
                log::warn!("umbriel_watcher: compositor error: {err}");
            }
            Ok(StreamMessage::Ignored) => {}
            Err(error) => {
                log::debug!("umbriel_watcher: ignore unparsed message: {error}");
            }
        }
    }
}

#[derive(Default)]
struct StreamState {
    workspaces: Option<Vec<UmbrielWorkspace>>,
    windows: Option<Vec<UmbrielWindow>>,
}

impl StreamState {
    fn apply(&mut self, event: StreamEvent) {
        match event {
            StreamEvent::Windows(windows) => self.windows = Some(windows),
            StreamEvent::Workspaces(workspaces) => self.workspaces = Some(workspaces),
        }
    }
}

#[derive(Debug, Deserialize)]
#[serde(untagged)]
enum StreamMessage {
    Event(StreamEvent),
    Error {
        err: String,
    },
    #[allow(dead_code)]
    Ignored,
}

#[derive(Debug, Deserialize)]
#[serde(tag = "event", content = "data", rename_all = "lowercase")]
enum StreamEvent {
    Windows(Vec<UmbrielWindow>),
    Workspaces(Vec<UmbrielWorkspace>),
}

#[derive(Debug, Default, Clone, PartialEq, Eq, Deserialize)]
struct UmbrielWorkspace {
    #[serde(default)]
    id: String,
    #[serde(default)]
    output: String,
    #[serde(default)]
    active: bool,
}

#[derive(Debug, Default, Clone, PartialEq, Eq, Deserialize)]
struct UmbrielWindow {
    #[serde(default)]
    app_id: String,
    #[serde(default)]
    title: String,
    #[serde(default)]
    output: String,
    #[serde(default)]
    workspace: String,
    #[serde(default)]
    active: bool,
    #[serde(default)]
    fullscreen: bool,
    #[serde(default)]
    maximized: bool,
    #[serde(default)]
    maximized_to_edges: bool,
    #[serde(default)]
    minimized: bool,
    #[serde(default)]
    visible: bool,
    #[serde(default)]
    tab_hidden: bool,
    #[serde(default)]
    pinned: bool,
}

fn collect_outputs_windows<'a>(
    registered_outputs: impl IntoIterator<Item = &'a str>,
    workspaces: &[UmbrielWorkspace],
    windows: &[UmbrielWindow],
) -> Vec<(String, Vec<Window>)> {
    let active_workspaces: HashSet<&str> = workspaces
        .iter()
        .filter(|ws| ws.active && !ws.id.is_empty())
        .map(|ws| ws.id.as_str())
        .collect();

    let mut by_output: HashMap<String, Vec<Window>> = HashMap::new();
    for name in registered_outputs {
        if !name.is_empty() {
            by_output.entry(name.to_string()).or_default();
        }
    }
    for ws in workspaces {
        if !ws.output.is_empty() {
            by_output.entry(ws.output.clone()).or_default();
        }
    }

    for window in windows {
        if window.output.is_empty() || window.minimized || window.tab_hidden || !window.visible {
            continue;
        }
        if !active_workspaces.is_empty()
            && !window.pinned
            && !window.workspace.is_empty()
            && !active_workspaces.contains(window.workspace.as_str())
        {
            continue;
        }

        by_output
            .entry(window.output.clone())
            .or_default()
            .push(Window {
                application_id: window.app_id.clone(),
                title: window.title.clone(),
                flags: window_to_flags(window),
            });
    }

    by_output.into_iter().collect()
}

fn window_to_flags(window: &UmbrielWindow) -> u32 {
    let mut flags = WAYWALLEN_WIN_HAS_NON_MINIMIZED;
    if window.active {
        flags |= WAYWALLEN_WIN_HAS_ACTIVE;
    }
    if window.maximized || window.maximized_to_edges {
        flags |= WAYWALLEN_WIN_HAS_MAXIMIZED;
    }
    if window.fullscreen {
        flags |= WAYWALLEN_WIN_HAS_FULLSCREEN;
    }
    flags
}
