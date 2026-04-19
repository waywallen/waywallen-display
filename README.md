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
| Qt 6 | optional | QML plugin, on by default |

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# without plugins
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DWAYWALLEN_DISPLAY_BUILD_PLUGINS=OFF
```

## Minimal example
```c
#include <waywallen_display.h>
#include <poll.h>

static void on_frame_ready(void *ud, const waywallen_frame_t *f) { /* ... */ }
static void on_disconnected(void *ud, int code, const char *msg)  { /* ... */ }

int main(void) {
    waywallen_display_callbacks_t cb = {
        .on_frame_ready  = on_frame_ready,
        .on_disconnected = on_disconnected,
        .user_data       = /* your state */ NULL,
    };
    waywallen_display_t *d = waywallen_display_new(&cb);

    /* 1. Non-blocking connect + drive the handshake state machine */
    waywallen_display_begin_connect(d, NULL, "my-display", 1920, 1080, 60000);
    int fd = waywallen_display_get_fd(d);
    for (;;) {
        int rc = waywallen_display_advance_handshake(d);
        if (rc == WAYWALLEN_HS_DONE) break;
        if (rc < 0) return 1;
        struct pollfd p = { .fd = fd, .events =
            rc == WAYWALLEN_HS_NEED_READ  ? POLLIN  :
            rc == WAYWALLEN_HS_NEED_WRITE ? POLLOUT : (POLLIN | POLLOUT) };
        poll(&p, 1, -1);
    }

    /* 2. Main loop: poll + dispatch */
    for (;;) {
        struct pollfd p = { .fd = fd, .events = POLLIN };
        poll(&p, 1, -1);
        if (p.revents & (POLLERR | POLLHUP)) break;
        if (p.revents & POLLIN && waywallen_display_dispatch(d) < 0) break;
        /* 3. Release each buffer once your GPU is done with it */
        /* waywallen_display_release_frame(d, idx, seq); */
    }

    /* 4. Cleanup */
    waywallen_display_disconnect(d);
    waywallen_display_destroy(d);
}
```

## Examples and plugins

- [`examples/minimal_egl.c`](examples/minimal_egl.c)
- [`plugins/qml/`](plugins/qml) — Qt 6 QML plugin exposing `Waywallen.Display` to QML.
