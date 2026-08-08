use std::collections::HashMap;
use std::io::{BufRead, BufReader};
use std::os::unix::net::UnixStream;
use std::path::PathBuf;
use std::process::Command;
use std::sync::Arc;
use std::thread;
use std::time::Duration;

use crate::watcher::{BindingRegistry, Command as WatcherCommand, CommandSender, OutputInfo};
use waywallen_display::{
    WAYWALLEN_WIN_HAS_ACTIVE, WAYWALLEN_WIN_HAS_FULLSCREEN, WAYWALLEN_WIN_HAS_MAXIMIZED,
    WAYWALLEN_WIN_HAS_NON_MINIMIZED,
};

pub fn detect_socket() -> Option<PathBuf> {
    let his = std::env::var_os("HYPRLAND_INSTANCE_SIGNATURE")?;
    let xdg = std::env::var_os("XDG_RUNTIME_DIR")?;
    let mut path = PathBuf::from(xdg);
    path.push("hypr");
    path.push(his);
    path.push(".socket2.sock");
    if path.exists() {
        Some(path)
    } else {
        None
    }
}

pub fn spawn(registry: BindingRegistry, commands: CommandSender) {
    let Some(sock) = detect_socket() else {
        return;
    };
    log::info!("hyprland_watcher: enabled (socket={})", sock.display());
    thread::spawn(move || run_loop(sock, registry, commands));
}

fn run_loop(socket_path: PathBuf, registry: BindingRegistry, commands: CommandSender) {
    loop {
        match UnixStream::connect(&socket_path) {
            Ok(stream) => {
                push_state(&registry, &commands);
                let reader = BufReader::new(stream);
                for line in reader.lines() {
                    match line {
                        Ok(_) => push_state(&registry, &commands),
                        Err(e) => {
                            log::warn!("hyprland_watcher: read error: {e}");
                            break;
                        }
                    }
                }
            }
            Err(e) => log::warn!("hyprland_watcher: connect {}: {e}", socket_path.display()),
        }
        thread::sleep(Duration::from_secs(2));
    }
}

fn push_state(registry: &BindingRegistry, commands: &CommandSender) {
    let snapshot = match hyprctl_snapshot() {
        Ok(v) => v,
        Err(e) => {
            log::warn!("hyprland_watcher: hyprctl: {e}");
            return;
        }
    };
    let by_output = aggregate_flags(&snapshot);
    let bindings: Vec<Arc<OutputInfo>> = registry.lock().unwrap().values().cloned().collect();
    for binding in bindings {
        let flags = by_output.get(binding.display_name()).copied().unwrap_or(0);
        if !binding.replace_window_flags(flags) {
            continue;
        }
        commands.send(WatcherCommand::WindowState {
            display_name: binding.display_name().to_string(),
            flags,
        });
    }
}

#[derive(serde::Deserialize)]
struct Client {
    address: String,
    monitor: i64,
    workspace: Workspace,
    fullscreen: i64,
    mapped: bool,
}

#[derive(serde::Deserialize)]
struct Workspace {
    id: i64,
}

#[derive(serde::Deserialize)]
struct Monitor {
    id: i64,
    name: String,
    #[serde(rename = "activeWorkspace")]
    active_workspace: WorkspaceRef,
}

#[derive(serde::Deserialize)]
struct WorkspaceRef {
    id: i64,
}

#[derive(serde::Deserialize)]
struct ActiveWindow {
    address: String,
}

struct Snapshot {
    clients: Vec<Client>,
    monitors: Vec<Monitor>,
    active_addr: Option<String>,
}

fn hyprctl_snapshot() -> anyhow::Result<Snapshot> {
    let clients = run_hyprctl_json::<Vec<Client>>(&["clients", "-j"])?;
    let monitors = run_hyprctl_json::<Vec<Monitor>>(&["monitors", "-j"])?;
    let active_addr = run_hyprctl_json::<ActiveWindow>(&["activewindow", "-j"])
        .ok()
        .map(|a| a.address)
        .filter(|s| !s.is_empty());
    Ok(Snapshot {
        clients,
        monitors,
        active_addr,
    })
}

fn run_hyprctl_json<T: serde::de::DeserializeOwned>(args: &[&str]) -> anyhow::Result<T> {
    let out = Command::new("hyprctl").args(args).output()?;
    if !out.status.success() {
        anyhow::bail!("hyprctl {args:?} exit {}", out.status);
    }
    Ok(serde_json::from_slice(&out.stdout)?)
}

fn aggregate_flags(snap: &Snapshot) -> HashMap<String, u32> {
    let mon_name: HashMap<i64, String> = snap
        .monitors
        .iter()
        .map(|m| (m.id, m.name.clone()))
        .collect();
    let active_ws: HashMap<i64, i64> = snap
        .monitors
        .iter()
        .map(|m| (m.id, m.active_workspace.id))
        .collect();
    let active = snap.active_addr.as_deref();
    let mut out: HashMap<String, u32> = HashMap::new();
    for c in &snap.clients {
        if !c.mapped {
            continue;
        }
        let Some(name) = mon_name.get(&c.monitor) else {
            continue;
        };
        if active_ws.get(&c.monitor) != Some(&c.workspace.id) {
            continue;
        }
        let entry = out.entry(name.clone()).or_insert(0);
        *entry |= WAYWALLEN_WIN_HAS_NON_MINIMIZED;
        if Some(c.address.as_str()) == active {
            *entry |= WAYWALLEN_WIN_HAS_ACTIVE;
        }
        match c.fullscreen {
            1 => *entry |= WAYWALLEN_WIN_HAS_MAXIMIZED,
            2 => *entry |= WAYWALLEN_WIN_HAS_FULLSCREEN,
            _ => {}
        }
    }
    out
}
