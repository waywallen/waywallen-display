//! EWMH window watcher for X11 sessions.
//!
//! Every EWMH window manager (Marco, Muffin, xfwm4, Openbox, KWin/X11, …)
//! publishes the managed windows on the root window as `_NET_CLIENT_LIST`,
//! the focused one as `_NET_ACTIVE_WINDOW` and the visible workspace as
//! `_NET_CURRENT_DESKTOP`; each client carries its own `_NET_WM_STATE` and
//! `_NET_WM_DESKTOP`. That is enough to fill the `WAYWALLEN_WIN_HAS_*` bits
//! per RandR monitor without any window-manager specific IPC.
//!
//! The watcher runs on its own X connection and thread, so the round trips
//! it makes never stall frame presentation.

use super::{Command, CommandSender, Window};
use std::collections::{HashMap, HashSet};
use std::time::Duration;
use waywallen_display::{
    WAYWALLEN_WIN_HAS_ACTIVE, WAYWALLEN_WIN_HAS_FULLSCREEN, WAYWALLEN_WIN_HAS_MAXIMIZED,
    WAYWALLEN_WIN_HAS_NON_MINIMIZED,
};
use x11rb::connection::Connection as _;
use x11rb::protocol::randr::{self, ConnectionExt as _};
use x11rb::protocol::xproto::{
    self, Atom, AtomEnum, ChangeWindowAttributesAux, ConnectionExt as _, EventMask,
};
use x11rb::protocol::Event;
use x11rb::xcb_ffi::XCBConnection;

/// Batches the bursts of property changes a single user action produces
/// (map + state + active + stacking) into one recomputation.
const SETTLE_DELAY: Duration = Duration::from_millis(40);

x11rb::atom_manager! {
    Atoms: AtomsCookie {
        _NET_CLIENT_LIST,
        _NET_ACTIVE_WINDOW,
        _NET_CURRENT_DESKTOP,
        _NET_WM_DESKTOP,
        _NET_WM_NAME,
        _NET_WM_STATE,
        _NET_WM_STATE_HIDDEN,
        _NET_WM_STATE_MAXIMIZED_VERT,
        _NET_WM_STATE_MAXIMIZED_HORZ,
        _NET_WM_STATE_FULLSCREEN,
        _NET_WM_WINDOW_TYPE,
        _NET_WM_WINDOW_TYPE_DESKTOP,
        _NET_WM_WINDOW_TYPE_DOCK,
        UTF8_STRING,
    }
}

/// `_NET_WM_DESKTOP` value of windows shown on every workspace.
const ALL_DESKTOPS: u32 = 0xFFFF_FFFF;

/// Our own desktop windows carry this class; they never count as clients.
const OWN_WM_CLASS: &str = "waywallen-x11";

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct WindowState {
    pub hidden: bool,
    pub active: bool,
    pub maximized_vert: bool,
    pub maximized_horz: bool,
    pub fullscreen: bool,
}

impl WindowState {
    pub fn to_flags(self) -> u32 {
        if self.hidden {
            return 0;
        }
        let mut flags = WAYWALLEN_WIN_HAS_NON_MINIMIZED;
        if self.active {
            flags |= WAYWALLEN_WIN_HAS_ACTIVE;
        }
        if self.maximized_vert && self.maximized_horz {
            flags |= WAYWALLEN_WIN_HAS_MAXIMIZED;
        }
        if self.fullscreen {
            flags |= WAYWALLEN_WIN_HAS_FULLSCREEN;
        }
        flags
    }
}

/// Whether a window on `window_desktop` is visible while `current` is the
/// shown workspace. Windows without the property, and window managers
/// without workspaces, count as visible.
pub fn on_current_desktop(window_desktop: Option<u32>, current: Option<u32>) -> bool {
    match (window_desktop, current) {
        (Some(ALL_DESKTOPS), _) | (None, _) | (_, None) => true,
        (Some(desktop), Some(current)) => desktop == current,
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Rect {
    pub x: i32,
    pub y: i32,
    pub width: i32,
    pub height: i32,
}

impl Rect {
    pub fn intersects(&self, other: &Rect) -> bool {
        self.x < other.x + other.width
            && other.x < self.x + self.width
            && self.y < other.y + other.height
            && other.y < self.y + self.height
    }
}

struct Monitor {
    name: String,
    rect: Rect,
}

struct Watcher {
    conn: XCBConnection,
    root: xproto::Window,
    atoms: Atoms,
    /// Client windows we already asked for PropertyNotify/ConfigureNotify.
    selected: HashSet<xproto::Window>,
    last_sent: HashMap<String, Vec<Window>>,
}

impl Watcher {
    fn property32(
        &self,
        window: xproto::Window,
        property: Atom,
        type_: impl Into<Atom>,
    ) -> Vec<u32> {
        self.conn
            .get_property(false, window, property, type_, 0, 1024)
            .ok()
            .and_then(|cookie| cookie.reply().ok())
            .and_then(|reply| reply.value32().map(|values| values.collect()))
            .unwrap_or_default()
    }

    fn property_text(
        &self,
        window: xproto::Window,
        property: Atom,
        type_: impl Into<Atom>,
    ) -> Vec<u8> {
        self.conn
            .get_property(false, window, property, type_, 0, 1024)
            .ok()
            .and_then(|cookie| cookie.reply().ok())
            .map(|reply| reply.value)
            .unwrap_or_default()
    }

    fn monitors(&self) -> Vec<Monitor> {
        let Some(reply) = self
            .conn
            .randr_get_monitors(self.root, true)
            .ok()
            .and_then(|cookie| cookie.reply().ok())
        else {
            return Vec::new();
        };
        reply
            .monitors
            .iter()
            .filter_map(|monitor| {
                let name = self
                    .conn
                    .get_atom_name(monitor.name)
                    .ok()?
                    .reply()
                    .ok()
                    .map(|reply| String::from_utf8_lossy(&reply.name).into_owned())?;
                Some(Monitor {
                    name,
                    rect: Rect {
                        x: i32::from(monitor.x),
                        y: i32::from(monitor.y),
                        width: i32::from(monitor.width),
                        height: i32::from(monitor.height),
                    },
                })
            })
            .collect()
    }

    fn select_client(&mut self, window: xproto::Window) {
        if !self.selected.insert(window) {
            return;
        }
        let aux = ChangeWindowAttributesAux::new()
            .event_mask(EventMask::PROPERTY_CHANGE | EventMask::STRUCTURE_NOTIFY);
        // The window may already be gone; the BadWindow error arrives as an
        // event and is ignored.
        let _ = self.conn.change_window_attributes(window, &aux);
    }

    fn client_rect(&self, window: xproto::Window) -> Option<Rect> {
        let geometry = self.conn.get_geometry(window).ok()?.reply().ok()?;
        let origin = self
            .conn
            .translate_coordinates(window, self.root, 0, 0)
            .ok()?
            .reply()
            .ok()?;
        Some(Rect {
            x: i32::from(origin.dst_x),
            y: i32::from(origin.dst_y),
            width: i32::from(geometry.width),
            height: i32::from(geometry.height),
        })
    }

    fn snapshot(&mut self) -> HashMap<String, Vec<Window>> {
        let monitors = self.monitors();
        let mut per_monitor: HashMap<String, Vec<Window>> = monitors
            .iter()
            .map(|monitor| (monitor.name.clone(), Vec::new()))
            .collect();
        let clients = self.property32(self.root, self.atoms._NET_CLIENT_LIST, AtomEnum::WINDOW);
        let active = self
            .property32(self.root, self.atoms._NET_ACTIVE_WINDOW, AtomEnum::WINDOW)
            .first()
            .copied()
            .unwrap_or(0);
        let current = self
            .property32(
                self.root,
                self.atoms._NET_CURRENT_DESKTOP,
                AtomEnum::CARDINAL,
            )
            .first()
            .copied();
        self.selected.retain(|window| clients.contains(window));
        for client in clients {
            self.select_client(client);
            let types = self.property32(client, self.atoms._NET_WM_WINDOW_TYPE, AtomEnum::ATOM);
            if types.contains(&self.atoms._NET_WM_WINDOW_TYPE_DESKTOP)
                || types.contains(&self.atoms._NET_WM_WINDOW_TYPE_DOCK)
            {
                continue;
            }
            let class = self.property_text(client, AtomEnum::WM_CLASS.into(), AtomEnum::STRING);
            let mut class_parts = class.split(|byte| *byte == 0);
            let instance = String::from_utf8_lossy(class_parts.next().unwrap_or_default());
            let application_id = class_parts
                .next()
                .map(|part| String::from_utf8_lossy(part).into_owned())
                .filter(|part| !part.is_empty())
                .unwrap_or_else(|| instance.clone().into_owned());
            if application_id == OWN_WM_CLASS {
                continue;
            }
            let desktop = self
                .property32(client, self.atoms._NET_WM_DESKTOP, AtomEnum::CARDINAL)
                .first()
                .copied();
            if !on_current_desktop(desktop, current) {
                continue;
            }
            let states = self.property32(client, self.atoms._NET_WM_STATE, AtomEnum::ATOM);
            let state = WindowState {
                hidden: states.contains(&self.atoms._NET_WM_STATE_HIDDEN),
                active: client == active,
                maximized_vert: states.contains(&self.atoms._NET_WM_STATE_MAXIMIZED_VERT),
                maximized_horz: states.contains(&self.atoms._NET_WM_STATE_MAXIMIZED_HORZ),
                fullscreen: states.contains(&self.atoms._NET_WM_STATE_FULLSCREEN),
            };
            let flags = state.to_flags();
            if flags == 0 {
                continue;
            }
            let Some(rect) = self.client_rect(client) else {
                continue;
            };
            let mut title =
                self.property_text(client, self.atoms._NET_WM_NAME, self.atoms.UTF8_STRING);
            if title.is_empty() {
                title = self.property_text(client, AtomEnum::WM_NAME.into(), AtomEnum::STRING);
            }
            let window = Window {
                application_id,
                title: String::from_utf8_lossy(&title).into_owned(),
                flags,
            };
            for monitor in &monitors {
                if monitor.rect.intersects(&rect) {
                    if let Some(windows) = per_monitor.get_mut(&monitor.name) {
                        windows.push(window.clone());
                    }
                }
            }
        }
        per_monitor
    }

    fn publish(&mut self, commands: &CommandSender) {
        let snapshot = self.snapshot();
        for (display_name, windows) in snapshot {
            if self.last_sent.get(&display_name) == Some(&windows) {
                continue;
            }
            log::debug!(
                "x11_watcher: {display_name}: {} window(s), flags=0x{:x}",
                windows.len(),
                windows.iter().fold(0, |flags, window| flags | window.flags)
            );
            self.last_sent.insert(display_name.clone(), windows.clone());
            commands.send(Command::WindowState {
                display_name,
                windows,
            });
        }
    }

    fn relevant(&self, event: &Event) -> bool {
        match event {
            Event::PropertyNotify(event) => {
                let atom = event.atom;
                if event.window == self.root {
                    atom == self.atoms._NET_CLIENT_LIST
                        || atom == self.atoms._NET_ACTIVE_WINDOW
                        || atom == self.atoms._NET_CURRENT_DESKTOP
                } else {
                    atom == self.atoms._NET_WM_STATE
                        || atom == self.atoms._NET_WM_DESKTOP
                        || atom == self.atoms._NET_WM_WINDOW_TYPE
                }
            }
            Event::ConfigureNotify(_)
            | Event::UnmapNotify(_)
            | Event::MapNotify(_)
            | Event::DestroyNotify(_)
            | Event::RandrScreenChangeNotify(_) => true,
            _ => false,
        }
    }

    fn run(mut self, commands: CommandSender) {
        self.publish(&commands);
        let _ = self.conn.flush();
        loop {
            let event = match self.conn.wait_for_event() {
                Ok(event) => event,
                Err(error) => {
                    log::warn!("x11_watcher: X connection lost: {error}");
                    return;
                }
            };
            let mut dirty = self.relevant(&event);
            std::thread::sleep(SETTLE_DELAY);
            loop {
                match self.conn.poll_for_event() {
                    Ok(Some(event)) => dirty |= self.relevant(&event),
                    Ok(None) => break,
                    Err(error) => {
                        log::warn!("x11_watcher: X connection lost: {error}");
                        return;
                    }
                }
            }
            if dirty {
                self.publish(&commands);
                let _ = self.conn.flush();
            }
        }
    }
}

/// Starts the EWMH watcher. Returns whether window observation is
/// available, i.e. the window manager advertises `_NET_CLIENT_LIST`.
pub fn spawn(commands: CommandSender) -> bool {
    let setup = (|| -> anyhow::Result<Watcher> {
        let (conn, screen_num) = XCBConnection::connect(None)?;
        let root = conn.setup().roots[screen_num].root;
        let atoms = Atoms::new(&conn)?.reply()?;
        conn.change_window_attributes(
            root,
            &ChangeWindowAttributesAux::new().event_mask(EventMask::PROPERTY_CHANGE),
        )?;
        conn.randr_select_input(root, randr::NotifyMask::SCREEN_CHANGE)?;
        Ok(Watcher {
            conn,
            root,
            atoms,
            selected: HashSet::new(),
            last_sent: HashMap::new(),
        })
    })();
    let watcher = match setup {
        Ok(watcher) => watcher,
        Err(error) => {
            log::warn!("x11_watcher: unavailable: {error:#}");
            return false;
        }
    };
    let has_client_list = watcher
        .conn
        .get_property(
            false,
            watcher.root,
            watcher.atoms._NET_CLIENT_LIST,
            AtomEnum::WINDOW,
            0,
            0,
        )
        .ok()
        .and_then(|cookie| cookie.reply().ok())
        .is_some_and(|reply| reply.type_ != u32::from(AtomEnum::NONE));
    if !has_client_list {
        log::warn!("x11_watcher: window manager does not publish _NET_CLIENT_LIST");
        return false;
    }
    match std::thread::Builder::new()
        .name("x11-watcher".into())
        .spawn(move || watcher.run(commands))
    {
        Ok(_) => {
            log::info!("x11_watcher: observing EWMH client windows");
            true
        }
        Err(error) => {
            log::warn!("x11_watcher: spawn thread failed: {error}");
            false
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn hidden_windows_carry_no_flags() {
        let state = WindowState {
            hidden: true,
            active: true,
            fullscreen: true,
            ..Default::default()
        };
        assert_eq!(state.to_flags(), 0);
    }

    #[test]
    fn maximized_needs_both_axes() {
        let half = WindowState {
            maximized_vert: true,
            ..Default::default()
        };
        assert_eq!(half.to_flags(), WAYWALLEN_WIN_HAS_NON_MINIMIZED);
        let full = WindowState {
            active: true,
            maximized_vert: true,
            maximized_horz: true,
            ..Default::default()
        };
        assert_eq!(
            full.to_flags(),
            WAYWALLEN_WIN_HAS_NON_MINIMIZED
                | WAYWALLEN_WIN_HAS_ACTIVE
                | WAYWALLEN_WIN_HAS_MAXIMIZED
        );
        let fullscreen = WindowState {
            fullscreen: true,
            ..Default::default()
        };
        assert_eq!(
            fullscreen.to_flags(),
            WAYWALLEN_WIN_HAS_NON_MINIMIZED | WAYWALLEN_WIN_HAS_FULLSCREEN
        );
    }

    #[test]
    fn workspace_visibility() {
        assert!(on_current_desktop(Some(1), Some(1)));
        assert!(!on_current_desktop(Some(0), Some(1)));
        assert!(on_current_desktop(Some(ALL_DESKTOPS), Some(1)));
        assert!(on_current_desktop(None, Some(1)));
        assert!(on_current_desktop(Some(3), None));
    }

    #[test]
    fn rect_intersection_excludes_touching_edges() {
        let left = Rect {
            x: 0,
            y: 0,
            width: 1920,
            height: 1080,
        };
        let right = Rect {
            x: 1920,
            y: 0,
            width: 1280,
            height: 1024,
        };
        let spanning = Rect {
            x: 1800,
            y: 100,
            width: 400,
            height: 300,
        };
        assert!(!left.intersects(&right));
        assert!(left.intersects(&spanning));
        assert!(right.intersects(&spanning));
    }
}
