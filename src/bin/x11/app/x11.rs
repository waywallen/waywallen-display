//! X server side of the client: monitor discovery over RandR, the GPU the
//! server renders on (DRI3), and the per-monitor desktop windows.

use crate::vulkan;
use anyhow::{bail, Context, Result};
use ash::vk;
use md5::{Digest, Md5};
use std::ffi::{c_void, CStr};
use std::os::fd::AsRawFd;
use x11rb::connection::Connection as _;
use x11rb::properties::{WmHints, WmSizeHints, WmSizeHintsSpecification};
use x11rb::protocol::dri3::ConnectionExt as _;
use x11rb::protocol::randr::{self, ConnectionExt as _};
use x11rb::protocol::xproto::{
    AtomEnum, ConfigureWindowAux, ConnectionExt as _, CreateWindowAux, EventMask, PropMode, Screen,
    Window, WindowClass,
};
use x11rb::wrapper::ConnectionExt as _;
use x11rb::xcb_ffi::XCBConnection;

/// The raw XCB handles `VK_KHR_xcb_surface` needs. The connection stays
/// owned by x11rb and outlives every Vulkan surface built on it.
#[derive(Clone, Copy)]
pub struct XcbTarget {
    pub connection: *mut c_void,
    pub visual: u32,
}

impl vulkan::WindowSystem for XcbTarget {
    type Target = Window;
    const NAME: &'static str = "X11";
    const APPLICATION: &'static CStr = c"waywallen-x11";
    const SURFACE_EXTENSION: &'static CStr = ash::khr::xcb_surface::NAME;

    fn create_surface(
        &self,
        entry: &ash::Entry,
        instance: &ash::Instance,
        window: &Window,
    ) -> Result<vk::SurfaceKHR> {
        if self.connection.is_null() || *window == 0 {
            bail!("x11rb did not expose a raw XCB connection/window");
        }
        let info = vk::XcbSurfaceCreateInfoKHR::default()
            .connection(self.connection)
            .window(*window);
        let loader = ash::khr::xcb_surface::Instance::new(entry, instance);
        unsafe { loader.create_xcb_surface(&info, None) }.context("vkCreateXcbSurfaceKHR")
    }

    fn presentation_support(
        &self,
        entry: &ash::Entry,
        instance: &ash::Instance,
        physical_device: vk::PhysicalDevice,
        queue_family: u32,
    ) -> bool {
        let loader = ash::khr::xcb_surface::Instance::new(entry, instance);
        unsafe {
            loader.get_physical_device_xcb_presentation_support(
                physical_device,
                queue_family,
                &mut *self.connection,
                self.visual,
            )
        }
    }
}

x11rb::atom_manager! {
    pub Atoms: AtomsCookie {
        _NET_WM_WINDOW_TYPE,
        _NET_WM_WINDOW_TYPE_DESKTOP,
        _NET_WM_STATE,
        _NET_WM_STATE_BELOW,
        _NET_WM_STATE_STICKY,
        _NET_WM_STATE_SKIP_TASKBAR,
        _NET_WM_STATE_SKIP_PAGER,
        _NET_WM_DESKTOP,
        _NET_WM_NAME,
        _NET_WM_PID,
        _NET_WM_USER_TIME,
        _NET_CLIENT_LIST_STACKING,
        _NET_RESTACK_WINDOW,
        _NET_SUPPORTED,
        CAJA_DESKTOP_WINDOW_ID,
        NAUTILUS_DESKTOP_WINDOW_ID,
        _MOTIF_WM_HINTS,
        UTF8_STRING,
        EDID,
    }
}

/// One RandR monitor, in root window coordinates.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Monitor {
    /// RandR monitor name, e.g. `eDP` or `HDMI-A-0`; also the display name
    /// registered with the daemon.
    pub name: String,
    pub x: i16,
    pub y: i16,
    pub width: u16,
    pub height: u16,
    pub refresh_mhz: u32,
    /// Stable identity of the panel behind the monitor (name + EDID ids),
    /// hashed into the daemon `instance_id`.
    pub identity: String,
}

impl Monitor {
    pub fn size(&self) -> (u32, u32) {
        (u32::from(self.width), u32::from(self.height))
    }

    pub fn instance_id(&self) -> String {
        let mut hasher = Md5::new();
        hasher.update(self.identity.as_bytes());
        format!("x11-{:x}", hasher.finalize())
    }
}

pub fn mode_refresh_mhz(mode: &randr::ModeInfo) -> u32 {
    let total = u64::from(mode.htotal) * u64::from(mode.vtotal);
    if total == 0 {
        return 0;
    }
    let mut refresh = u64::from(mode.dot_clock) * 1000 / total;
    if mode.mode_flags.contains(randr::ModeFlag::DOUBLE_SCAN) {
        refresh /= 2;
    }
    if mode.mode_flags.contains(randr::ModeFlag::INTERLACE) {
        refresh *= 2;
    }
    u32::try_from(refresh).unwrap_or(0)
}

/// Parses the PNP manufacturer id, product code and serial out of an EDID
/// base block.
pub fn edid_identity(edid: &[u8]) -> Option<String> {
    if edid.len() < 16 || edid[..8] != [0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00] {
        return None;
    }
    let vendor = u16::from_be_bytes([edid[8], edid[9]]);
    let letter = |shift: u16| char::from(b'@' + ((vendor >> shift) & 0x1f) as u8);
    let make: String = [letter(10), letter(5), letter(0)].into_iter().collect();
    let product = u16::from_le_bytes([edid[10], edid[11]]);
    let serial = u32::from_le_bytes([edid[12], edid[13], edid[14], edid[15]]);
    Some(format!(
        "make={make}|model={product:04x}|serial={serial:08x}"
    ))
}

fn output_edid(conn: &XCBConnection, atoms: &Atoms, output: randr::Output) -> Option<String> {
    let reply = conn
        .randr_get_output_property(output, atoms.EDID, AtomEnum::ANY, 0, 64, false, false)
        .ok()?
        .reply()
        .ok()?;
    edid_identity(&reply.data)
}

/// Lists the active RandR monitors. Falls back to one monitor spanning the
/// whole screen when the server predates RandR 1.5.
pub fn query_monitors(
    conn: &XCBConnection,
    screen: &Screen,
    atoms: &Atoms,
) -> Result<Vec<Monitor>> {
    let whole_screen = || Monitor {
        name: "screen".into(),
        x: 0,
        y: 0,
        width: screen.width_in_pixels,
        height: screen.height_in_pixels,
        refresh_mhz: 60_000,
        identity: "name=screen".into(),
    };
    let version = conn
        .randr_query_version(1, 5)
        .context("RRQueryVersion")?
        .reply()
        .context("RRQueryVersion reply")?;
    if (version.major_version, version.minor_version) < (1, 5) {
        log::warn!(
            "RandR {}.{} has no monitors; using the whole screen",
            version.major_version,
            version.minor_version
        );
        return Ok(vec![whole_screen()]);
    }
    let resources = conn
        .randr_get_screen_resources_current(screen.root)
        .context("RRGetScreenResourcesCurrent")?
        .reply()
        .context("RRGetScreenResourcesCurrent reply")?;
    let monitors = conn
        .randr_get_monitors(screen.root, true)
        .context("RRGetMonitors")?
        .reply()
        .context("RRGetMonitors reply")?;
    let mut out = Vec::with_capacity(monitors.monitors.len());
    for monitor in &monitors.monitors {
        let name = conn
            .get_atom_name(monitor.name)
            .context("GetAtomName")?
            .reply()
            .map(|reply| String::from_utf8_lossy(&reply.name).into_owned())
            .unwrap_or_default();
        let mut refresh_mhz = 0;
        let mut identity = format!("name={name}");
        if let Some(output) = monitor.outputs.first().copied() {
            if let Some(edid) = output_edid(conn, atoms, output) {
                identity.push('|');
                identity.push_str(&edid);
            }
            let crtc = conn
                .randr_get_output_info(output, resources.config_timestamp)
                .ok()
                .and_then(|cookie| cookie.reply().ok())
                .map(|info| info.crtc)
                .unwrap_or(0);
            if crtc != 0 {
                if let Some(info) = conn
                    .randr_get_crtc_info(crtc, resources.config_timestamp)
                    .ok()
                    .and_then(|cookie| cookie.reply().ok())
                {
                    if let Some(mode) = resources.modes.iter().find(|mode| mode.id == info.mode) {
                        refresh_mhz = mode_refresh_mhz(mode);
                    }
                }
            }
        }
        if monitor.width == 0 || monitor.height == 0 {
            continue;
        }
        out.push(Monitor {
            name,
            x: monitor.x,
            y: monitor.y,
            width: monitor.width,
            height: monitor.height,
            refresh_mhz: if refresh_mhz > 0 { refresh_mhz } else { 60_000 },
            identity,
        });
    }
    if out.is_empty() {
        out.push(whole_screen());
    }
    Ok(out)
}

/// The DRM render node of the GPU the X server renders on, as
/// `(major, minor)`. DRI3 may hand out the primary node; it is mapped to the
/// render node through sysfs. `(0, 0)` when DRI3 is missing, which leaves
/// device selection to the importer requirements alone.
pub fn server_drm_device(conn: &XCBConnection, root: Window) -> (u32, u32) {
    let opened = (|| -> Result<(u32, u32)> {
        conn.dri3_query_version(1, 0)?.reply()?;
        let reply = conn.dri3_open(root, 0)?.reply()?;
        let mut stat = std::mem::MaybeUninit::<libc::stat>::zeroed();
        if unsafe { libc::fstat(reply.device_fd.as_raw_fd(), stat.as_mut_ptr()) } != 0 {
            return Err(std::io::Error::last_os_error().into());
        }
        let stat = unsafe { stat.assume_init() };
        if stat.st_mode & libc::S_IFMT != libc::S_IFCHR {
            anyhow::bail!("DRI3 fd is not a character device");
        }
        Ok(render_node(
            libc::major(stat.st_rdev),
            libc::minor(stat.st_rdev),
        ))
    })();
    match opened {
        Ok(device) => {
            log::info!(
                "DRI3: X server renders on DRM node {}:{}",
                device.0,
                device.1
            );
            device
        }
        Err(error) => {
            log::warn!("DRI3: cannot identify the server GPU: {error:#}");
            (0, 0)
        }
    }
}

/// The render node next to a DRM node, or the node itself when it already
/// is one or sysfs does not list one.
fn render_node(major: u32, minor: u32) -> (u32, u32) {
    let dir = format!("/sys/dev/char/{major}:{minor}/device/drm");
    let Ok(entries) = std::fs::read_dir(dir) else {
        return (major, minor);
    };
    for entry in entries.flatten() {
        if !entry.file_name().to_string_lossy().starts_with("renderD") {
            continue;
        }
        let Ok(dev) = std::fs::read_to_string(entry.path().join("dev")) else {
            continue;
        };
        if let Some(node) = parse_dev(&dev) {
            return node;
        }
    }
    (major, minor)
}

fn parse_dev(dev: &str) -> Option<(u32, u32)> {
    let (major, minor) = dev.trim().split_once(':')?;
    Some((major.parse().ok()?, minor.parse().ok()?))
}

/// Creates the borderless, sticky, always-below desktop window for one
/// monitor. Every hint is set before the window is mapped, because window
/// managers read `_NET_WM_WINDOW_TYPE`/`_NET_WM_STATE` only at map time.
pub fn create_desktop_window(
    conn: &XCBConnection,
    screen: &Screen,
    atoms: &Atoms,
    monitor: &Monitor,
) -> Result<Window> {
    let window = conn.generate_id().context("allocate window id")?;
    let aux = CreateWindowAux::new()
        .background_pixel(screen.black_pixel)
        .event_mask(
            EventMask::STRUCTURE_NOTIFY
                | EventMask::EXPOSURE
                | EventMask::BUTTON_PRESS
                | EventMask::BUTTON_RELEASE
                | EventMask::POINTER_MOTION,
        );
    conn.create_window(
        x11rb::COPY_DEPTH_FROM_PARENT,
        window,
        screen.root,
        monitor.x,
        monitor.y,
        monitor.width,
        monitor.height,
        0,
        WindowClass::INPUT_OUTPUT,
        x11rb::COPY_FROM_PARENT,
        &aux,
    )
    .context("CreateWindow")?;

    let title = format!("waywallen {}", monitor.name);
    conn.change_property8(
        PropMode::REPLACE,
        window,
        AtomEnum::WM_NAME,
        AtomEnum::STRING,
        title.as_bytes(),
    )?;
    conn.change_property8(
        PropMode::REPLACE,
        window,
        atoms._NET_WM_NAME,
        atoms.UTF8_STRING,
        title.as_bytes(),
    )?;
    conn.change_property8(
        PropMode::REPLACE,
        window,
        AtomEnum::WM_CLASS,
        AtomEnum::STRING,
        b"waywallen-x11\0waywallen-x11\0",
    )?;
    conn.change_property32(
        PropMode::REPLACE,
        window,
        atoms._NET_WM_PID,
        AtomEnum::CARDINAL,
        &[std::process::id()],
    )?;
    conn.change_property32(
        PropMode::REPLACE,
        window,
        atoms._NET_WM_WINDOW_TYPE,
        AtomEnum::ATOM,
        &[atoms._NET_WM_WINDOW_TYPE_DESKTOP],
    )?;
    conn.change_property32(
        PropMode::REPLACE,
        window,
        atoms._NET_WM_STATE,
        AtomEnum::ATOM,
        &[
            atoms._NET_WM_STATE_BELOW,
            atoms._NET_WM_STATE_STICKY,
            atoms._NET_WM_STATE_SKIP_TASKBAR,
            atoms._NET_WM_STATE_SKIP_PAGER,
        ],
    )?;
    conn.change_property32(
        PropMode::REPLACE,
        window,
        atoms._NET_WM_DESKTOP,
        AtomEnum::CARDINAL,
        &[0xFFFF_FFFF],
    )?;
    // _MOTIF_WM_HINTS: flags = MWM_HINTS_DECORATIONS, decorations = none.
    conn.change_property32(
        PropMode::REPLACE,
        window,
        atoms._MOTIF_WM_HINTS,
        atoms._MOTIF_WM_HINTS,
        &[2, 0, 0, 0, 0],
    )?;
    let mut hints = WmHints::new();
    hints.input = Some(false);
    hints.set(conn, window)?;
    let position = (
        WmSizeHintsSpecification::UserSpecified,
        i32::from(monitor.x),
        i32::from(monitor.y),
    );
    let size = (
        WmSizeHintsSpecification::UserSpecified,
        i32::from(monitor.width),
        i32::from(monitor.height),
    );
    let mut size_hints = WmSizeHints::new();
    size_hints.position = Some(position);
    size_hints.size = Some(size);
    size_hints.min_size = Some((i32::from(monitor.width), i32::from(monitor.height)));
    size_hints.max_size = Some((i32::from(monitor.width), i32::from(monitor.height)));
    size_hints.set_normal_hints(conn, window)?;

    conn.map_window(window).context("MapWindow")?;
    Ok(window)
}

/// Moves an existing desktop window onto a monitor's new geometry.
pub fn place_desktop_window(conn: &XCBConnection, window: Window, monitor: &Monitor) -> Result<()> {
    let mut size_hints = WmSizeHints::new();
    size_hints.position = Some((
        WmSizeHintsSpecification::UserSpecified,
        i32::from(monitor.x),
        i32::from(monitor.y),
    ));
    size_hints.size = Some((
        WmSizeHintsSpecification::UserSpecified,
        i32::from(monitor.width),
        i32::from(monitor.height),
    ));
    size_hints.min_size = Some((i32::from(monitor.width), i32::from(monitor.height)));
    size_hints.max_size = Some((i32::from(monitor.width), i32::from(monitor.height)));
    size_hints.set_normal_hints(conn, window)?;
    conn.configure_window(
        window,
        &ConfigureWindowAux::new()
            .x(i32::from(monitor.x))
            .y(i32::from(monitor.y))
            .width(u32::from(monitor.width))
            .height(u32::from(monitor.height)),
    )?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn edid_identity_reads_pnp_vendor_product_and_serial() {
        let mut edid = vec![0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00];
        // "BOE" = 0b00010_01111_00101.
        edid.extend_from_slice(&[0x09, 0xe5, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12]);
        assert_eq!(
            edid_identity(&edid).as_deref(),
            Some("make=BOE|model=1234|serial=12345678")
        );
        assert_eq!(edid_identity(&edid[..10]), None);
        edid[0] = 1;
        assert_eq!(edid_identity(&edid), None);
    }

    #[test]
    fn sysfs_dev_numbers_parse() {
        assert_eq!(parse_dev("226:128\n"), Some((226, 128)));
        assert_eq!(parse_dev("226"), None);
        assert_eq!(parse_dev("x:1"), None);
    }

    #[test]
    fn mode_refresh_is_reported_in_millihertz() {
        let mode = randr::ModeInfo {
            id: 1,
            width: 1920,
            height: 1080,
            dot_clock: 148_500_000,
            hsync_start: 2008,
            hsync_end: 2052,
            htotal: 2200,
            hskew: 0,
            vsync_start: 1084,
            vsync_end: 1089,
            vtotal: 1125,
            name_len: 0,
            mode_flags: randr::ModeFlag::from(0u32),
        };
        assert_eq!(mode_refresh_mhz(&mode), 60_000);
    }
}
