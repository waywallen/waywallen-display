//! waywallen-layer-shell — Wayland layer-shell wallpaper client.
//!
//! Connects to a Wayland compositor that supports `zwlr_layer_shell_v1`
//! (Hyprland, Sway, Niri, River, …) and registers each output as a
//! display with the daemon over the waywallen-display UDS protocol.

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

const GETTEXT_DOMAIN: &str = "waywallen-layer-shell";

fn init_gettext() {
    gettextrs::setlocale(gettextrs::LocaleCategory::LcAll, "");

    let locale_dir = std::env::var_os("WAYWALLEN_LOCALEDIR")
        .map(PathBuf::from)
        .or_else(|| {
            let exe_dir = std::env::current_exe().ok()?.parent()?.to_path_buf();
            let sibling = exe_dir.join("share/locale");
            if sibling.is_dir() {
                return Some(sibling);
            }
            let prefix = exe_dir.parent()?.join("share/locale");
            prefix.is_dir().then_some(prefix)
        });
    if let Some(dir) = locale_dir {
        if let Err(e) = gettextrs::bindtextdomain(GETTEXT_DOMAIN, dir) {
            log::debug!("bindtextdomain failed: {e}");
        }
    }
    if let Err(e) = gettextrs::bind_textdomain_codeset(GETTEXT_DOMAIN, "UTF-8") {
        log::debug!("bind_textdomain_codeset failed: {e}");
    }
    if let Err(e) = gettextrs::textdomain(GETTEXT_DOMAIN) {
        log::debug!("textdomain failed: {e}");
    }
}

fn usage() -> ! {
    eprintln!(
        "{}",
        gettextrs::gettext(
            "usage: waywallen-layer-shell [--socket PATH] [--name STR] [--version]\n\
             \n\
             Environment:\n\
               WAYWALLEN_SOCKET   fallback UDS path when --socket is omitted\n\
               WAYLAND_DISPLAY    required — picks the compositor to attach to"
        )
    );
    std::process::exit(2);
}

fn main() -> Result<()> {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info")).init();
    init_gettext();

    let mut socket: Option<PathBuf> = None;
    let mut name_prefix = String::from("output");
    let mut it = std::env::args().skip(1);
    while let Some(arg) = it.next() {
        match arg.as_str() {
            "--socket" => {
                socket = it.next().map(PathBuf::from);
                if socket.is_none() {
                    eprintln!("{}", gettextrs::gettext("--socket requires a value"));
                    usage();
                }
            }
            "--name" => {
                name_prefix = it.next().unwrap_or_else(|| {
                    eprintln!("{}", gettextrs::gettext("--name requires a value"));
                    usage();
                });
            }
            "--version" => {
                println!(
                    "waywallen-layer-shell {}.{}.{}",
                    sys::WAYWALLEN_DISPLAY_VERSION_MAJOR,
                    sys::WAYWALLEN_DISPLAY_VERSION_MINOR,
                    sys::WAYWALLEN_DISPLAY_VERSION_PATCH
                );
                return Ok(());
            }
            "-h" | "--help" => usage(),
            other => {
                eprintln!("{}: {other}", gettextrs::gettext("unknown argument"));
                usage();
            }
        }
    }
    let socket = socket
        .or_else(|| std::env::var_os("WAYWALLEN_SOCKET").map(PathBuf::from))
        .unwrap_or_else(default_socket_path);

    app::run(socket, name_prefix)
}
