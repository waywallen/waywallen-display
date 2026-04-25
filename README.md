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

## Build

| Dependency | Required | Notes |
|------------|----------|-------|
| CMake ≥ 3.16 | ✓ | |
| GCC | ✓ | |
| Qt 6 | optional | QML plugin |

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DWAYWALLEN_DISPLAY_PLUGIN_QML=ON
cmake --build build
sudo cmake --install build
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
| [kde](./extensions/kde) | Require QML Plugin |