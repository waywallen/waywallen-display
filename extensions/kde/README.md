# waywallen-kde

Plasma 6 Wallpaper plugin for `waywallen`.  

## Runtime dependencies

- `waywallen` daemon.  
- The `waywallen-display` library with qml

## install
```sh
kpackagetool6 --type Plasma/Wallpaper -i package
# or -u to upgrade, -r to remove
```

After upgrading, restart plasmashell so the new QML module is picked up:

```sh
systemctl --user restart plasma-plasmashell.service
```

### Translations troubleshooting

Plasma 6.5.6 and newer should load translations directly from the installed
wallpaper package. If the settings remain in English, check the Plasma version
first:

```sh
plasmashell --version
```

For an older Plasma version or a confirmed package-local translation
regression, copy the complete locale tree from the installed wallpaper into
the standard per-user locale directory:

```sh
data_home="${XDG_DATA_HOME:-$HOME/.local/share}"
mkdir -p "$data_home/locale"
cp -r "$data_home/plasma/wallpapers/org.waywallen.kde/contents/locale/." \
    "$data_home/locale/"
systemctl --user restart plasma-plasmashell.service
```

This is a workaround, not required for a working package-local installation.
To remove every externally copied Waywallen catalog without affecting other
translations:

```sh
data_home="${XDG_DATA_HOME:-$HOME/.local/share}"
find "$data_home/locale" -type f \
    -name 'plasma_wallpaper_org.waywallen.kde.mo' -delete
systemctl --user restart plasma-plasmashell.service
```

### X11 sessions (e.g. Steam Deck)

On X11, plasmashell needs the EGL XCB backend to import the wallpaper's
DMA-BUFs. Add a systemd user drop-in at
`~/.config/systemd/user/plasma-plasmashell.service.d/override.conf`:

```ini
[Service]
Environment=QT_XCB_GL_INTEGRATION=xcb_egl
```

Then reload and restart:

```sh
systemctl --user daemon-reload
systemctl --user restart plasma-plasmashell.service
```
