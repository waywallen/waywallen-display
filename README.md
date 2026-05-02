# waywallen-display

A C client library for the `waywallen-display` protocol, exposing a stable C ABI
for Linux desktop environments (KDE Plasma, GNOME Shell, ...) to talk to a
`waywallen` daemon.

## Flow

```
┌───── waywallen daemon (separate process) ─────┐                ┌──── desktop integration process ────┐
│                                               │    v1 UDS      │                                     │
│  wallpaper subprocess dma-buf ─▶ dispatcher  │  ◀──────▶   │       waywallen-display             │
│                                               │   msgs + fd    │             │                       │
└───────────────────────────────────────────────┘                │             ▼                       │
                                                                 │     render / composition            │
                                                                 └─────────────────────────────────────┘

socket path: $XDG_RUNTIME_DIR/waywallen/display.sock
```

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
