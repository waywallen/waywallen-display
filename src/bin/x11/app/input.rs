//! Pointer input on the desktop windows, forwarded to the daemon in the
//! same units the layer-shell client uses.
//!
//! Core events reach our windows only where nothing covers them. A file
//! manager's icon window covers the whole monitor and takes every click,
//! even where it is transparent (nemo-desktop), so pointer motion over it
//! is followed through XI2 raw motion on the root window plus QueryPointer
//! and forwarded as hover. Buttons are left to the icon window: it cannot
//! tell us whether a click landed on an icon or on empty desktop.

use super::{x11::Monitor, App};
use std::time::{Duration, Instant};
use waywallen_display as sys;
use x11rb::connection::RequestConnection as _;
use x11rb::protocol::xinput::{self, ConnectionExt as _};
use x11rb::protocol::xproto::{ButtonPressEvent, ConnectionExt as _, MotionNotifyEvent, Window};
use x11rb::xcb_ffi::XCBConnection;

/// Hover updates over icon windows are coalesced to this interval; raw
/// motion arrives at the mouse's report rate.
pub const POINTER_INTERVAL_MS: i32 = 8;

/// `XIAllMasterDevices`.
const ALL_MASTER_DEVICES: u16 = 1;

#[derive(Default)]
pub struct PointerForwarding {
    /// Raw motion arrived since the last QueryPointer.
    pub moved: bool,
    last_query: Option<Instant>,
    last_sent: Option<(Window, i16, i16)>,
}

/// Asks for XI2 raw motion on the root window. Without XInput 2 hover over
/// icon windows is simply not forwarded.
pub fn select_raw_motion(conn: &XCBConnection, root: Window) {
    let selected = (|| -> anyhow::Result<()> {
        conn.extension_information(xinput::X11_EXTENSION_NAME)?
            .ok_or_else(|| anyhow::anyhow!("no XInputExtension"))?;
        let version = conn.xinput_xi_query_version(2, 0)?.reply()?;
        if version.major_version < 2 {
            anyhow::bail!("XInput {}.{}", version.major_version, version.minor_version);
        }
        conn.xinput_xi_select_events(
            root,
            &[xinput::EventMask {
                deviceid: ALL_MASTER_DEVICES,
                mask: vec![xinput::XIEventMask::RAW_MOTION],
            }],
        )?;
        Ok(())
    })();
    if let Err(error) = selected {
        log::warn!("XI2 raw motion unavailable, no hover under icon windows: {error:#}");
    }
}

/// The monitor-local position of a root point, if it is on `monitor`.
pub fn monitor_local(monitor: &Monitor, root_x: i16, root_y: i16) -> Option<(i16, i16)> {
    let x = i32::from(root_x) - i32::from(monitor.x);
    let y = i32::from(root_y) - i32::from(monitor.y);
    ((0..i32::from(monitor.width)).contains(&x) && (0..i32::from(monitor.height)).contains(&y))
        .then_some((x as i16, y as i16))
}

/// linux/input-event-codes.h
const BTN_LEFT: u32 = 0x110;
const BTN_RIGHT: u32 = 0x111;
const BTN_MIDDLE: u32 = 0x112;
const BTN_SIDE: u32 = 0x113;
const BTN_EXTRA: u32 = 0x114;

/// One wheel notch. libinput reports a notch as 15 on wl_pointer.axis and
/// the layer-shell client divides that by 10.
const WHEEL_NOTCH: f32 = 1.5;

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum PointerAction {
    Button(u32),
    Scroll { dx: f32, dy: f32 },
}

/// Maps a core X11 button number to an evdev button code or a wheel step.
pub fn map_button(detail: u8) -> Option<PointerAction> {
    Some(match detail {
        1 => PointerAction::Button(BTN_LEFT),
        2 => PointerAction::Button(BTN_MIDDLE),
        3 => PointerAction::Button(BTN_RIGHT),
        4 => PointerAction::Scroll {
            dx: 0.0,
            dy: -WHEEL_NOTCH,
        },
        5 => PointerAction::Scroll {
            dx: 0.0,
            dy: WHEEL_NOTCH,
        },
        6 => PointerAction::Scroll {
            dx: -WHEEL_NOTCH,
            dy: 0.0,
        },
        7 => PointerAction::Scroll {
            dx: WHEEL_NOTCH,
            dy: 0.0,
        },
        8 => PointerAction::Button(BTN_SIDE),
        9 => PointerAction::Button(BTN_EXTRA),
        _ => return None,
    })
}

impl App {
    /// Forwards the pointer position as motion when it hovers an icon
    /// window; over our own windows core MotionNotify already does that.
    pub(super) fn forward_pointer_over_icons(&mut self) {
        if !self.pointer.moved || self.stacking.icon_toplevels.is_empty() {
            self.pointer.moved = false;
            return;
        }
        let interval = Duration::from_millis(POINTER_INTERVAL_MS as u64);
        if self
            .pointer
            .last_query
            .is_some_and(|last| last.elapsed() < interval)
        {
            return;
        }
        self.pointer.moved = false;
        self.pointer.last_query = Some(Instant::now());
        let Some(reply) = self
            .conn
            .query_pointer(self.screen.root)
            .ok()
            .and_then(|cookie| cookie.reply().ok())
        else {
            return;
        };
        if !reply.same_screen || !self.stacking.icon_toplevels.contains(&reply.child) {
            return;
        }
        let target = self.outputs.iter().find_map(|(window, entry)| {
            monitor_local(&entry.monitor, reply.root_x, reply.root_y).map(|(x, y)| (*window, x, y))
        });
        let Some((window, x, y)) = target else {
            return;
        };
        if self.pointer.last_sent == Some((window, x, y)) {
            return;
        }
        let Some((name, display)) = self.ready_display(window) else {
            return;
        };
        let rc = unsafe {
            sys::waywallen_display_send_pointer_motion(display, f32::from(x), f32::from(y), 0, 0)
        };
        if rc < 0 {
            log::debug!("[{name}] send pointer_motion (under icons) failed: {rc}");
        } else {
            log::trace!("[{name}] pointer over icon window at ({x}, {y})");
            self.pointer.last_sent = Some((window, x, y));
        }
    }

    fn ready_display(&self, window: u32) -> Option<(&str, *mut sys::waywallen_display_t)> {
        let entry = self.outputs.get(&window)?;
        let binding = entry.binding.as_ref()?;
        let session = entry
            .session
            .as_ref()
            .filter(|session| session.is_ready())?;
        Some((binding.display_name.as_str(), session.display.0))
    }

    pub(super) fn handle_motion(&self, event: &MotionNotifyEvent) {
        let Some((name, display)) = self.ready_display(event.event) else {
            return;
        };
        // Core event times wrap at 32 bits; let the display library stamp
        // CLOCK_MONOTONIC instead, as layer-shell does.
        let rc = unsafe {
            sys::waywallen_display_send_pointer_motion(
                display,
                f32::from(event.event_x),
                f32::from(event.event_y),
                0,
                0,
            )
        };
        if rc < 0 {
            log::debug!("[{name}] send pointer_motion failed: {rc}");
        }
    }

    pub(super) fn handle_button(&self, event: &ButtonPressEvent, pressed: bool) {
        let Some((name, display)) = self.ready_display(event.event) else {
            return;
        };
        let (x, y) = (f32::from(event.event_x), f32::from(event.event_y));
        let rc = match map_button(event.detail) {
            Some(PointerAction::Button(button)) => {
                let state = if pressed {
                    sys::WAYWALLEN_POINTER_BUTTON_STATE_PRESSED
                } else {
                    sys::WAYWALLEN_POINTER_BUTTON_STATE_RELEASED
                };
                unsafe {
                    sys::waywallen_display_send_pointer_button(display, x, y, button, state, 0, 0)
                }
            }
            // The server reports every wheel step as a press/release pair.
            Some(PointerAction::Scroll { dx, dy }) if pressed => unsafe {
                sys::waywallen_display_send_pointer_axis(
                    display,
                    x,
                    y,
                    dx,
                    dy,
                    sys::WAYWALLEN_POINTER_AXIS_SOURCE_WHEEL,
                    0,
                    0,
                )
            },
            _ => return,
        };
        if rc < 0 {
            log::debug!("[{name}] send pointer input failed: {rc}");
        } else {
            log::trace!(
                "[{name}] pointer button {} {} at ({x}, {y})",
                event.detail,
                if pressed { "pressed" } else { "released" }
            );
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn root_points_map_to_the_monitor_under_them() {
        let right = Monitor {
            name: "HDMI-A-0".into(),
            x: 1920,
            y: 0,
            width: 1280,
            height: 1024,
            refresh_mhz: 60_000,
            identity: String::new(),
        };
        assert_eq!(monitor_local(&right, 1920, 0), Some((0, 0)));
        assert_eq!(monitor_local(&right, 3199, 1023), Some((1279, 1023)));
        assert_eq!(monitor_local(&right, 1919, 10), None);
        assert_eq!(monitor_local(&right, 3200, 10), None);
        assert_eq!(monitor_local(&right, 2000, 1024), None);
    }

    #[test]
    fn core_buttons_map_to_evdev_codes_and_wheel_steps() {
        assert_eq!(map_button(1), Some(PointerAction::Button(BTN_LEFT)));
        assert_eq!(map_button(2), Some(PointerAction::Button(BTN_MIDDLE)));
        assert_eq!(map_button(3), Some(PointerAction::Button(BTN_RIGHT)));
        assert_eq!(
            map_button(5),
            Some(PointerAction::Scroll {
                dx: 0.0,
                dy: WHEEL_NOTCH
            })
        );
        assert_eq!(map_button(9), Some(PointerAction::Button(BTN_EXTRA)));
        assert_eq!(map_button(12), None);
    }
}
