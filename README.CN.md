# waywallen-display

`waywallen-display` 协议的 C 库，提供一份稳定的 C ABI  
给 LinuxDE（KDE Plasma、GNOME Shell、...）提供一个稳定的 C ABI，用来连到 `waywallen` daemon 进程

## 流程图

```
┌───── waywallen daemon (独立进程) ─────┐                ┌────  桌面集成进程  ────┐
│                                      │    v1 UDS      │                        │
│  壁纸渲染器子进程 ──dma-buf──▶ 分发器 │◀────────────▶│   waywallen-display   │
│                                       │  消息 + fd     │           │           │
└───────────────────────────────────────┘                │          ▼            │
                                                         │       渲染 / 合成     │
                                                         └──────────────────────┘

socket 路径：$XDG_RUNTIME_DIR/waywallen/display.sock
```

## 构建

| 依赖 | 必需 | 说明 |
|------|------|------|
| CMake ≥ 3.16 | ✓ | |
| GCC | ✓ | |
| Qt 6 | 可选 | QML 插件，默认开启； |

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# 不构建插件
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DWAYWALLEN_DISPLAY_BUILD_PLUGINS=OFF
```

## 最小示例
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

    /* 1. 非阻塞 connect + 推进握手状态机 */
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

    /* 2. 主循环：poll + dispatch */
    for (;;) {
        struct pollfd p = { .fd = fd, .events = POLLIN };
        poll(&p, 1, -1);
        if (p.revents & (POLLERR | POLLHUP)) break;
        if (p.revents & POLLIN && waywallen_display_dispatch(d) < 0) break;
        /* 3. GPU 用完每一帧后归还 buffer */
        /* waywallen_display_release_frame(d, idx, seq); */
    }

    /* 4. 清理 */
    waywallen_display_disconnect(d);
    waywallen_display_destroy(d);
}
```

## 示例与插件

- [`examples/minimal_egl.c`](examples/minimal_egl.c)  
- [`plugins/qml/`](plugins/qml) — Qt 6 QML 插件，让 `Waywallen.Display` 在 QML 里直接可用
