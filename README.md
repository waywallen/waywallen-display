# waywallen-display

Desktop integration for the `waywallen` wallpaper daemon — lets KDE Plasma,
GNOME Shell, and other Linux desktop shells display `waywallen` wallpaper
output as a regular surface, with zero-copy GPU sharing via DMA-BUF.

## What's implemented

- **Protocol client** — C library that speaks `waywallen-display` v1 to the
  daemon and receives DMA-BUF frames plus acquire/release sync fences.
- **Qt 6 QML plugin** (`Waywallen.Display`) — drop-in `WaywallenSurface` item
  for Qt Quick scenes.
- **gobject plugin** — Helper for building `GdkTexture` through `waywallen-display`.
- **KDE Plasma wallpaper extension** — Plasma 6 kpackage built on the QML
  plugin.
- **GNOME Shell extension** — Shell 48+ extension built on gobject plugin.
- **Wayland layer-shell client** — standalone
  wallpaper client for compositors that expose `zwlr_layer_shell_v1`.

## Install

Prebuilt artifacts are published on the
[GitHub releases page](https://github.com/waywallen/waywallen-display/releases).

### KDE Plasma extension

Download `waywallen-kde-<version>-<arch>-embed.zip` from the latest release, then:

```sh
kpackagetool6 --type Plasma/Wallpaper -i waywallen-kde-<version>-<arch>.zip
# -u to upgrade, -r to remove
```

> [!TIP]
> On KDE Plasma, restart the shell so the freshly installed QML plugin is picked up:
>
> ```sh
> systemctl --user restart plasma-plasmashell.service
> ```

> [!TIP]
> On X11(like steamdeck), plasmashell needs the EGL XCB backend, Add a systemd user drop-in at
> `~/.config/systemd/user/plasma-plasmashell.service.d/override.conf`:
> 
> ```ini
> [Service]
> Environment=QT_XCB_GL_INTEGRATION=xcb_egl
> ```
> 
> Then reload and restart:
> 
> ```sh
> systemctl --user daemon-reload
> systemctl --user restart plasma-plasmashell.service
> ```

### GNOME Shell extension

GNOME Shell on Wayland (tested on Shell 50). Download `waywallen-gnome-<version>-<arch>.zip` from the
latest release, then:

```sh
gnome-extensions install --force waywallen-gnome-<version>-<arch>.zip
gnome-extensions enable org.waywallen.gnome@waywallen.io
```

Log back out and in to load it.

### Wayland layer-shell client

For Wayland compositors that expose `zwlr_layer_shell_v1` (for example
Hyprland, Sway, Niri), download
`waywallen-layer-shell-<version>-<arch>.tar.gz` from the latest release, then:

```sh
tar -xzf waywallen-layer-shell-<version>-<arch>.tar.gz
install -Dm755 waywallen-layer-shell ~/.local/bin/waywallen-layer-shell
```

```sh
waywallen-layer-shell
# optional:
waywallen-layer-shell --socket "$XDG_RUNTIME_DIR/waywallen/display.sock"
```

> [!NOTE]  
> The appimage version of waywallen has already embed waywallen-layer-shell.  
> And waywallen will manage it.

## Extensions

| Extension | Notes |
|------------|----------|
| [kde](./extensions/kde) | Plasma 6 wallpaper plugin |
| [gnome](./extensions/gnome) | GNOME Shell 48–50 extension (Wayland) |
| layer-shell | Client for wayland `zwlr_layer_shell_v1` |

## Building from source

See [BUILD.md](./BUILD.md).
