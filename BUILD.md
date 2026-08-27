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
| GNU gettext tools | translations | `xgettext`, `msginit`, `msgmerge`, `msgfmt` |

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
| `WAYWALLEN_DISPLAY_WITH_VULKAN` | auto | Vulkan backend (DMA-BUF import via external-memory, DMA-BUF, DRM modifier, and foreign queue-family extensions). Defaults `ON` when the `vulkan` pkg-config module is present. |
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

## Translations

English source strings are the fallback and must remain in the source code.
Do not replace them with translated text. Runtime packages contain compiled
`.mo` files only; `.po` and `.pot` files remain in the source repository.

| Component | Text domain | Sources | POT | PO |
|-----------|-------------|---------|-----|----|
| KDE Plasma wallpaper | `plasma_wallpaper_org.waywallen.kde` | `extensions/kde/package/contents/ui/` | `extensions/kde/po/plasma_wallpaper_org.waywallen.kde.pot` | `extensions/kde/po/<lang>.po` |
| GNOME Shell extension | `waywallen-gnome` | `extensions/gnome/extension/`, schema, renderer, metadata extraction input | `extensions/gnome/po/waywallen-gnome.pot` | `extensions/gnome/po/<lang>.po` |
| layer-shell CLI | `waywallen-layer-shell` | `src/bin/layer_shell/main.rs` | `po/layer-shell/waywallen-layer-shell.pot` | `po/layer-shell/<lang>.po` |

Update each POT and merge it into every existing PO catalog:

```bash
sh extensions/kde/Messages.sh
sh extensions/gnome/Messages.sh
sh po/layer-shell/Messages.sh
```

The scripts work from any directory, use component allowlists, run `msgmerge`
for existing translations, and validate the result. To add a language, add its
locale code to the component's `LINGUAS`, then run its `Messages.sh`; `msginit`
creates `<lang>.po`. Translate that file without changing commands, paths,
URLs, protocol names, placeholders, or product names.

Validate catalogs directly with:

```bash
msgfmt --check --check-format -o /dev/null extensions/kde/po/ru.po
msgfmt --check --check-format -o /dev/null extensions/gnome/po/ru.po
msgfmt --check --check-format -o /dev/null po/layer-shell/ru.po
```

CMake compiles all languages listed in `LINGUAS`:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target waywallen-layer-shell-translations
```

The layer-shell executable is built by Cargo, not CMake. A conventional
installation places catalogs in `${CMAKE_INSTALL_LOCALEDIR}`, which GNU gettext
searches by default for standard system prefixes. The release archive keeps
`waywallen-layer-shell` at its root and places catalogs in the sibling
`share/locale` tree. Other relocatable or non-standard layouts must set
`WAYWALLEN_LOCALEDIR` when that relationship is not preserved.

The KDE and GNOME translation targets are enabled with their corresponding
plugin options. Release helpers compile and package them automatically:

```bash
./scripts/build_kde_embed.sh
./scripts/build_gnome_zip.sh
```

KDE testing:

```bash
kpackagetool6 --type Plasma/Wallpaper --install build/waywallen-kde-<version>-<arch>-embed.zip
LANGUAGE=ru LANG=ru_RU.UTF-8 plasmoidviewer -a org.waywallen.kde
```

Plasma loads the catalog from
`contents/locale/ru/LC_MESSAGES/plasma_wallpaper_org.waywallen.kde.mo`.
Package-local wallpaper translations require Plasma 6.5.6 or newer. If the
wallpaper configuration dialog still does not register the catalog, confirm
the Plasma/libplasma version and report the Plasma regression. A distribution
package may additionally install the same catalog through the normal CMake
install target (`${CMAKE_INSTALL_LOCALEDIR}`); do not add a custom PO/MO loader
to QML.

GNOME testing:

```bash
gnome-extensions install --force build/waywallen-gnome-<version>-<arch>.zip
LANGUAGE=ru LANG=ru_RU.UTF-8 gnome-extensions prefs org.waywallen.gnome@waywallen.io
```

GNOME Shell loads `locale/ru/LC_MESSAGES/waywallen-gnome.mo` from the
extension directory. Log out and back in after installing an updated runtime
extension if the running Shell has cached it.

CLI testing with a staged relocatable layout:

```bash
mkdir -p test-root/share/locale/ru/LC_MESSAGES
cp target/release/waywallen-layer-shell test-root/
msgfmt -o test-root/share/locale/ru/LC_MESSAGES/waywallen-layer-shell.mo po/layer-shell/ru.po
LANGUAGE=ru LANG=ru_RU.UTF-8 test-root/waywallen-layer-shell --help
LANGUAGE=de LANG=de_DE.UTF-8 test-root/waywallen-layer-shell --help
```

The first command prints Russian help; the unsupported German locale falls
back to the English source strings. For compatibility with the existing CLI,
`--help` writes to standard error and exits with status 2.

The external `waywallen` AppImage build is not part of this repository. If it
bundles `waywallen-layer-shell`, it must also bundle
`share/locale/<lang>/LC_MESSAGES/waywallen-layer-shell.mo` alongside the
archive-style root executable, or set `WAYWALLEN_LOCALEDIR` to the bundled
locale directory.

## Logging

`libwaywallen_display` writes INFO, WARN, and ERROR messages asynchronously to
daily log files alongside the daemon:

```text
$XDG_STATE_HOME/waywallen/logs/waywallen_display_rYYYY-MM-DD.log
~/.local/state/waywallen/logs/waywallen_display_rYYYY-MM-DD.log   # fallback
```

- **Levels:** INFO, WARN, and ERROR are persisted; DEBUG is not written to the
  file (stderr / host callback only).
- **Async:** application threads enqueue lines into a fixed ring buffer and
  never perform disk I/O. A dedicated flush thread writes buffered lines every
  two seconds.
- **Non-blocking:** if the enqueue lock is contended or the ring is full, lines
  may be dropped rather than blocking compositor or render threads.
- **Retention:** at most the seven newest `waywallen_display_r*.log` files are
  kept; older files are removed on flush.
- **Tags:** backends may call `waywallen_display_set_log_tag()` once at init
  (`kde-plasma`, `gnome-shell`, `layer-shell`) to prefix log lines.

For bug reports, attach both `waywallen_r*.log` (daemon) and
`waywallen_display_r*.log` (display) from the same log directory.
