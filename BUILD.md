# Build

This document is for developers building `waywallen-display` from source. End
users should grab a prebuilt release from the
[GitHub releases page](https://github.com/waywallen/waywallen-display/releases).

## Architecture

`waywallen-display` is a C client library that exposes a stable C ABI to the
desktop integration process. It speaks the `waywallen-display` v1 wire
protocol over a Unix domain socket to the `waywallen` daemon.

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

## Dependencies

| Dependency | Required | Notes |
|------------|----------|-------|
| CMake ≥ 3.16 | ✓ | |
| C compiler (GCC / Clang) | ✓ | |
| `pkg-config` | ✓ | locates `egl` / `glesv2` / `vulkan` headers |
| `egl`, `glesv2` headers | EGL backend | runtime libs are `dlopen`-ed |
| `vulkan` headers | Vulkan backend | runtime lib is `dlopen`-ed |
| Qt 6 (Quick, Gui, Qml, DBus) | QML plugin | `WAYWALLEN_DISPLAY_PLUGIN_QML=ON` |

## Quick start

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
sudo cmake --install build
```

## CMake options

| Option | Default | Description |
|--------|---------|-------------|
| `CMAKE_BUILD_TYPE` | `Debug` | Standard CMake build type. Use `Release` for release builds. |
| `CMAKE_INSTALL_PREFIX` | system | Standard CMake install prefix. |
| `WAYWALLEN_DISPLAY_WITH_EGL` | `ON` | EGL backend (DMA-BUF import via `EGL_EXT_image_dma_buf_import`). |
| `WAYWALLEN_DISPLAY_WITH_VULKAN` | auto | Vulkan backend (DMA-BUF import via `VK_KHR_external_memory_fd`). Defaults `ON` when the `vulkan` pkg-config module is present. |
| `WAYWALLEN_DISPLAY_PLUGIN_QML` | `OFF` | Build the Qt 6 QML plugin (`Waywallen.Display`). |
| `WAYWALLEN_DISPLAY_PLUGIN_GOBJECT` | `OFF` | Build the GObject Introspection plugin. |
| `WAYWALLEN_DISPLAY_BUILD_TESTS` | `OFF` | Build unit tests under `tests/`. |
| `WAYWALLEN_DISPLAY_BUILD_EXAMPLES` | `OFF` | Build the example programs under `examples/`. |
| `WAYWALLEN_DISPLAY_REGEN_PROTO` | `OFF` | Regenerate `src/generated/ww_proto.{h,c}` from `waywallen_display_v1.xml`. Maintainer-only; requires the sibling `waywallen/tools/wayproto-gen`. |

## Packaging the KDE extension zip

With `WAYWALLEN_DISPLAY_PLUGIN_QML=ON`, the build produces a self-contained
KDE Plasma kpackage zip via CPack:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DWAYWALLEN_DISPLAY_PLUGIN_QML=ON
cmake --build build --target package
# → build/waywallen-kde-<version>-<arch>.zip
```

Each desktop-shell extension is exposed as a CPack component. Adding a new one
(e.g. GNOME Shell) only requires another `install(... COMPONENT <name>
EXCLUDE_FROM_ALL)` block plus `list(APPEND CPACK_COMPONENTS_ALL <name>)` —
`cmake --build build --target package` then emits one zip per component.
