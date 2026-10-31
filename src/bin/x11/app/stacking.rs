//! Keeps the wallpaper windows between the desktop background and the
//! file manager's icon window.
//!
//! Desktop shells draw the icons in their own `_NET_WM_WINDOW_TYPE_DESKTOP`
//! window: Caja (MATE), nemo-desktop (Cinnamon), xfdesktop (XFCE),
//! pcmanfm. Window managers put every desktop-type window into the same
//! bottom layer and order it by map time, and Caja and Nemo lower their
//! window on map, so the order between them and us is a race. Cinnamon on
//! Wayland adds a third party: csd-background draws the background into a
//! keep-below desktop window under Xwayland, and that one has to stay
//! underneath us.
//!
//! The rule, evaluated over `_NET_CLIENT_LIST_STACKING` (bottom to top)
//! every time it changes: each of our windows must sit below the lowest
//! icon window and above the highest background window under it. Windows
//! out of place are restacked with `_NET_RESTACK_WINDOW` when the window
//! manager supports it, and with a sibling ConfigureWindow request
//! otherwise.

use super::App;
use std::collections::{HashMap, HashSet};
use x11rb::cookie::Cookie;
use x11rb::protocol::xproto::{
    AtomEnum, ClientMessageEvent, ConfigureWindowAux, ConnectionExt as _, EventMask,
    GetPropertyReply, PropMode, StackMode, Window,
};
use x11rb::wrapper::ConnectionExt as _;
use x11rb::xcb_ffi::XCBConnection;

/// `WM_CLASS` (instance or class) of the windows that draw only the
/// background and therefore belong underneath the wallpaper.
const BACKGROUND_CLASSES: &[&str] = &["csd-background"];

/// `_NET_RESTACK_WINDOW` source indication: a pager or other direct user
/// action, which window managers honour without focus-stealing checks.
const SOURCE_PAGER: u32 = 2;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Role {
    /// One of our wallpaper windows.
    Own,
    /// A file manager's desktop window with the icons on it.
    Icons,
    /// A window that paints the plain desktop background.
    Background,
    /// Any regular client; irrelevant for the order.
    Other,
}

/// Properties of a foreign client that decide its role.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ClientInfo {
    pub desktop_type: bool,
    pub instance: String,
    pub class: String,
}

impl ClientInfo {
    fn matches(&self, names: &[&str]) -> bool {
        names.iter().any(|name| {
            self.instance.eq_ignore_ascii_case(name) || self.class.eq_ignore_ascii_case(name)
        })
    }
}

/// Classifies a foreign window. `announced` is set when the root window
/// names it as the file manager's desktop (`CAJA_DESKTOP_WINDOW_ID`,
/// `NAUTILUS_DESKTOP_WINDOW_ID`). Every other desktop-type window counts as
/// an icon window unless it is a known background painter, so desktop
/// shells we do not know by name are kept on top of the wallpaper.
pub fn classify(info: &ClientInfo, announced: bool) -> Role {
    if announced {
        Role::Icons
    } else if !info.desktop_type {
        Role::Other
    } else if info.matches(BACKGROUND_CLASSES) {
        Role::Background
    } else {
        Role::Icons
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Restack {
    Below(Window),
    Above(Window),
}

impl std::fmt::Display for Restack {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Restack::Below(sibling) => write!(f, "below 0x{sibling:x}"),
            Restack::Above(sibling) => write!(f, "above 0x{sibling:x}"),
        }
    }
}

/// Given the stacking order bottom to top, returns the restack requests
/// that put every own window right below the lowest icon window and above
/// the background windows beneath it. Without an icon window the own
/// windows only have to clear the highest background window. Windows that
/// already satisfy the rule are left alone, so applying the result and
/// re-planning converges to an empty list.
pub fn plan_restack(stack: &[(Window, Role)]) -> Vec<(Window, Restack)> {
    let lowest_icons = stack.iter().position(|(_, role)| *role == Role::Icons);
    let background_limit = lowest_icons.unwrap_or(stack.len());
    let highest_background = stack[..background_limit]
        .iter()
        .rposition(|(_, role)| *role == Role::Background);
    let anchor = match (lowest_icons, highest_background) {
        (Some(icons), _) => Restack::Below(stack[icons].0),
        (None, Some(background)) => Restack::Above(stack[background].0),
        (None, None) => return Vec::new(),
    };
    stack
        .iter()
        .enumerate()
        .filter(|(_, (_, role))| *role == Role::Own)
        .filter(|(index, _)| {
            let under_icons = lowest_icons.is_none_or(|icons| *index < icons);
            let over_background = highest_background.is_none_or(|background| *index > background);
            !(under_icons && over_background)
        })
        .map(|(_, (window, _))| (*window, anchor))
        .collect()
}

type PropertyCookie<'c> = Cookie<'c, XCBConnection, GetPropertyReply>;

fn values32(cookie: Option<PropertyCookie<'_>>) -> Vec<u32> {
    cookie
        .and_then(|cookie| cookie.reply().ok())
        .and_then(|reply| reply.value32().map(|values| values.collect()))
        .unwrap_or_default()
}

/// Stacking bookkeeping of the main connection.
#[derive(Default)]
pub struct Stacking {
    /// Foreign clients seen in `_NET_CLIENT_LIST_STACKING`, by window id.
    clients: HashMap<Window, ClientInfo>,
    /// Root children (frames, or the windows themselves when undecorated)
    /// of the icon windows, as `QueryPointer` reports them.
    pub icon_toplevels: HashSet<Window>,
    /// Root child of each client, looked up once per client.
    toplevels: HashMap<Window, Window>,
    /// Server time of the last stacking-related root property change;
    /// set when the order has to be re-checked.
    pub dirty_at: Option<u32>,
    last_logged: Vec<(Window, Role)>,
}

impl App {
    /// Re-evaluates the stacking order if a root property change asked for
    /// it, and restacks our windows that are out of place.
    pub(super) fn sync_stacking(&mut self) {
        let Some(time) = self.stacking.dirty_at.take() else {
            return;
        };
        let root = self.screen.root;
        let atoms = self.atoms;
        let window_list = |property| {
            self.conn
                .get_property(false, root, property, AtomEnum::WINDOW, 0, 4096)
                .ok()
        };
        let stacking_cookie = window_list(atoms._NET_CLIENT_LIST_STACKING);
        let caja_cookie = window_list(atoms.CAJA_DESKTOP_WINDOW_ID);
        let nautilus_cookie = window_list(atoms.NAUTILUS_DESKTOP_WINDOW_ID);
        let supported_cookie = self
            .conn
            .get_property(false, root, atoms._NET_SUPPORTED, AtomEnum::ATOM, 0, 4096)
            .ok();
        let stacking = values32(stacking_cookie);
        let mut announced = values32(caja_cookie);
        announced.extend(values32(nautilus_cookie));
        let restack_supported = values32(supported_cookie).contains(&atoms._NET_RESTACK_WINDOW);

        self.stacking
            .clients
            .retain(|window, _| stacking.contains(window));
        self.stacking
            .toplevels
            .retain(|window, _| stacking.contains(window));
        let unknown: Vec<Window> = stacking
            .iter()
            .copied()
            .filter(|window| {
                !self.outputs.contains_key(window) && !self.stacking.clients.contains_key(window)
            })
            .collect();
        let cookies: Vec<_> = unknown
            .iter()
            .map(|window| {
                (
                    *window,
                    self.conn
                        .get_property(
                            false,
                            *window,
                            atoms._NET_WM_WINDOW_TYPE,
                            AtomEnum::ATOM,
                            0,
                            64,
                        )
                        .ok(),
                    self.conn
                        .get_property(false, *window, AtomEnum::WM_CLASS, AtomEnum::STRING, 0, 256)
                        .ok(),
                )
            })
            .collect();
        for (window, types, class) in cookies {
            let desktop_type = types
                .and_then(|cookie| cookie.reply().ok())
                .and_then(|reply| {
                    reply.value32().map(|mut values| {
                        values.any(|atom| atom == atoms._NET_WM_WINDOW_TYPE_DESKTOP)
                    })
                })
                .unwrap_or(false);
            let class = class
                .and_then(|cookie| cookie.reply().ok())
                .map(|reply| reply.value)
                .unwrap_or_default();
            let mut parts = class.split(|byte| *byte == 0);
            let instance = String::from_utf8_lossy(parts.next().unwrap_or_default()).into_owned();
            let class = String::from_utf8_lossy(parts.next().unwrap_or_default()).into_owned();
            self.stacking.clients.insert(
                window,
                ClientInfo {
                    desktop_type,
                    instance,
                    class,
                },
            );
        }

        let stack: Vec<(Window, Role)> = stacking
            .iter()
            .map(|window| {
                let role = if self.outputs.contains_key(window) {
                    Role::Own
                } else {
                    self.stacking
                        .clients
                        .get(window)
                        .map(|info| classify(info, announced.contains(window)))
                        .unwrap_or(Role::Other)
                };
                (*window, role)
            })
            .collect();
        let desktop_stack: Vec<(Window, Role)> = stack
            .iter()
            .copied()
            .filter(|(_, role)| *role != Role::Other)
            .collect();
        if desktop_stack != self.stacking.last_logged {
            let described: Vec<String> = desktop_stack
                .iter()
                .map(|(window, role)| {
                    let class = self
                        .stacking
                        .clients
                        .get(window)
                        .map(|info| info.instance.as_str())
                        .unwrap_or("waywallen-x11");
                    format!("0x{window:x}={role:?}({class})")
                })
                .collect();
            log::info!("stacking (bottom to top): {}", described.join(" "));
            self.stacking.last_logged = desktop_stack;
        }

        let icons: Vec<Window> = stack
            .iter()
            .filter(|(_, role)| *role == Role::Icons)
            .map(|(window, _)| *window)
            .collect();
        self.stacking.icon_toplevels.clear();
        for window in icons {
            let toplevel = match self.stacking.toplevels.get(&window) {
                Some(toplevel) => Some(*toplevel),
                None => self.root_child(window),
            };
            if let Some(toplevel) = toplevel {
                self.stacking.toplevels.insert(window, toplevel);
                self.stacking.icon_toplevels.insert(toplevel);
            }
        }

        for (window, restack) in plan_restack(&stack) {
            log::info!(
                "restacking desktop window 0x{window:x} {restack} ({})",
                if restack_supported {
                    "_NET_RESTACK_WINDOW"
                } else {
                    "ConfigureWindow"
                }
            );
            if let Err(error) = self.request_restack(window, restack, restack_supported, time) {
                log::warn!("restack desktop window 0x{window:x}: {error}");
            }
        }
    }

    fn request_restack(
        &self,
        window: Window,
        restack: Restack,
        restack_supported: bool,
        time: u32,
    ) -> Result<(), x11rb::errors::ConnectionError> {
        let (sibling, mode) = match restack {
            Restack::Below(sibling) => (sibling, StackMode::BELOW),
            Restack::Above(sibling) => (sibling, StackMode::ABOVE),
        };
        if restack_supported {
            let event = ClientMessageEvent::new(
                32,
                window,
                self.atoms._NET_RESTACK_WINDOW,
                [SOURCE_PAGER, sibling, u32::from(mode), 0, 0],
            );
            self.conn.send_event(
                false,
                self.screen.root,
                EventMask::SUBSTRUCTURE_REDIRECT | EventMask::SUBSTRUCTURE_NOTIFY,
                event,
            )?;
        } else {
            // Metacity-derived window managers (Marco) drop stacking
            // requests from windows whose user time predates the active
            // window's; a fresh server time keeps the request valid. They
            // also ignore the sibling and lower to the bottom of the
            // desktop layer, which is still below the icon window.
            self.conn.change_property32(
                PropMode::REPLACE,
                window,
                self.atoms._NET_WM_USER_TIME,
                AtomEnum::CARDINAL,
                &[time],
            )?;
            self.conn.configure_window(
                window,
                &ConfigureWindowAux::new().sibling(sibling).stack_mode(mode),
            )?;
        }
        Ok(())
    }

    /// The ancestor of `window` that is a direct child of the root.
    fn root_child(&self, mut window: Window) -> Option<Window> {
        for _ in 0..8 {
            let tree = self.conn.query_tree(window).ok()?.reply().ok()?;
            if tree.parent == self.screen.root || tree.parent == 0 {
                return Some(window);
            }
            window = tree.parent;
        }
        None
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn info(desktop_type: bool, instance: &str, class: &str) -> ClientInfo {
        ClientInfo {
            desktop_type,
            instance: instance.into(),
            class: class.into(),
        }
    }

    #[test]
    fn desktop_windows_are_icons_unless_they_only_paint_the_background() {
        assert_eq!(classify(&info(true, "caja", "Caja"), false), Role::Icons);
        assert_eq!(
            classify(&info(true, "nemo-desktop", "Nemo-desktop"), false),
            Role::Icons
        );
        assert_eq!(
            classify(&info(true, "xfdesktop", "Xfdesktop"), false),
            Role::Icons
        );
        assert_eq!(
            classify(&info(true, "csd-background", "Csd-background"), false),
            Role::Background
        );
        assert_eq!(classify(&info(false, "caja", "Caja"), false), Role::Other);
        // The root property wins even before the type hint is readable.
        assert_eq!(classify(&info(false, "", ""), true), Role::Icons);
    }

    #[test]
    fn own_window_moves_below_the_icons() {
        use Role::*;
        // Caja lowered itself on map and now sits under us.
        let stack = [(0xa0, Icons), (0x10, Own), (0x30, Other)];
        assert_eq!(plan_restack(&stack), vec![(0x10, Restack::Below(0xa0))]);
        let settled = [(0x10, Own), (0xa0, Icons), (0x30, Other)];
        assert!(plan_restack(&settled).is_empty());
    }

    #[test]
    fn own_window_stays_between_background_and_icons() {
        use Role::*;
        // Cinnamon on Wayland: csd-background, us, nemo-desktop.
        let settled = [(0xb0, Background), (0x10, Own), (0xc0, Icons)];
        assert!(plan_restack(&settled).is_empty());
        let under_background = [(0x10, Own), (0xb0, Background), (0xc0, Icons)];
        assert_eq!(
            plan_restack(&under_background),
            vec![(0x10, Restack::Below(0xc0))]
        );
        let over_icons = [(0xb0, Background), (0xc0, Icons), (0x10, Own)];
        assert_eq!(
            plan_restack(&over_icons),
            vec![(0x10, Restack::Below(0xc0))]
        );
    }

    #[test]
    fn without_icons_own_windows_only_clear_the_background() {
        use Role::*;
        let stack = [
            (0x10, Own),
            (0x11, Own),
            (0xb0, Background),
            (0xb1, Background),
        ];
        assert_eq!(
            plan_restack(&stack),
            vec![(0x10, Restack::Above(0xb1)), (0x11, Restack::Above(0xb1)),]
        );
        assert!(plan_restack(&[(0xb0, Background), (0x10, Own)]).is_empty());
        assert!(plan_restack(&[(0x10, Own), (0x30, Other)]).is_empty());
    }

    #[test]
    fn every_monitor_window_goes_under_the_lowest_icon_window() {
        use Role::*;
        // nemo-desktop maps one icon window per monitor.
        let stack = [(0xc0, Icons), (0x10, Own), (0xc1, Icons), (0x11, Own)];
        assert_eq!(
            plan_restack(&stack),
            vec![(0x10, Restack::Below(0xc0)), (0x11, Restack::Below(0xc0)),]
        );
    }

    #[test]
    fn background_above_the_icons_is_ignored() {
        use Role::*;
        let stack = [(0xc0, Icons), (0xb0, Background), (0x10, Own)];
        assert_eq!(plan_restack(&stack), vec![(0x10, Restack::Below(0xc0))]);
        assert!(plan_restack(&[(0x10, Own), (0xc0, Icons), (0xb0, Background)]).is_empty());
    }
}
