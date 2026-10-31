//! waywallen-x11 — X11 wallpaper client.
//!
//! Places one `_NET_WM_WINDOW_TYPE_DESKTOP` window under every RandR monitor
//! of an EWMH window manager (Marco, Muffin, xfwm4, …) and registers each
//! monitor as a display with the daemon over the waywallen-display UDS
//! protocol. Frames are imported and presented exactly like the layer-shell
//! client does, through a Vulkan `VK_KHR_xcb_surface` swapchain.

mod app;
mod watcher;

use anyhow::Result;
use std::path::PathBuf;
use waywallen_display as sys;
use waywallen_display::client::vulkan;

fn default_socket_path() -> PathBuf {
    let runtime = std::env::var_os("XDG_RUNTIME_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("/tmp"));
    runtime.join("waywallen").join("display.sock")
}

fn usage() -> ! {
    eprintln!(
        "usage: waywallen-x11 [--socket PATH] [--name STR] [--version]\n\
         \n\
         Environment:\n\
           WAYWALLEN_SOCKET   fallback UDS path when --socket is omitted\n\
           DISPLAY            required — picks the X server to attach to"
    );
    std::process::exit(2);
}

fn main() -> Result<()> {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info")).init();

    let mut socket: Option<PathBuf> = None;
    let mut name_prefix = String::from("output");
    let mut it = std::env::args().skip(1);
    while let Some(arg) = it.next() {
        match arg.as_str() {
            "--socket" => {
                socket = it.next().map(PathBuf::from);
                if socket.is_none() {
                    eprintln!("--socket requires a value");
                    usage();
                }
            }
            "--name" => {
                name_prefix = it.next().unwrap_or_else(|| {
                    eprintln!("--name requires a value");
                    usage();
                });
            }
            "--version" => {
                println!(
                    "waywallen-x11 {}.{}.{}",
                    sys::WAYWALLEN_DISPLAY_VERSION_MAJOR,
                    sys::WAYWALLEN_DISPLAY_VERSION_MINOR,
                    sys::WAYWALLEN_DISPLAY_VERSION_PATCH
                );
                return Ok(());
            }
            "-h" | "--help" => usage(),
            other => {
                eprintln!("unknown argument: {other}");
                usage();
            }
        }
    }
    let socket = socket
        .or_else(|| std::env::var_os("WAYWALLEN_SOCKET").map(PathBuf::from))
        .unwrap_or_else(default_socket_path);

    app::run(socket, name_prefix)
}
