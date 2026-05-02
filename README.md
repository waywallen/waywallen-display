# waywallen-display

Desktop integration for the `waywallen` wallpaper daemon — lets KDE Plasma,
GNOME Shell, and other Linux desktop shells display `waywallen` wallpaper
output as a regular surface, with zero-copy GPU sharing via DMA-BUF.

## What's implemented

- **Protocol client** — C library that speaks `waywallen-display` v1 to the
  daemon and receives DMA-BUF frames plus acquire/release sync fences.
- **EGL backend** — imports DMA-BUFs as `EGLImage` via
  `EGL_EXT_image_dma_buf_import`.
- **Vulkan backend** — imports DMA-BUFs as `VkImage` via
  `VK_KHR_external_memory_fd`.
- **Qt 6 QML plugin** (`Waywallen.Display`) — drop-in `WaywallenSurface` item
  for Qt Quick scenes.
- **KDE Plasma wallpaper extension** — Plasma 6 kpackage built on the QML
  plugin.

GNOME Shell extension is on the roadmap.

## Install

Prebuilt artifacts are published on the
[GitHub releases page](https://github.com/waywallen/waywallen-display/releases).

### KDE Plasma extension

Download `waywallen-kde-<version>-<arch>.zip` from the latest release, then:

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

## Extensions

| Extension | Notes |
|------------|----------|
| [kde](./extensions/kde) | Plasma 6 wallpaper plugin |

## Building from source

See [BUILD.md](./BUILD.md).
