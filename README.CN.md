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
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DWAYWALLEN_DISPLAY_PLUGIN_QML=ON
cmake --build build
sudo cmake --install build
```

## 扩展
| 扩展 | 说明 |
|------------|----------|
| [kde](./extensions/kde) | 需要 QML Plugin |