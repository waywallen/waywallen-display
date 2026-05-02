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

## 安装

预编译产物发布在
[GitHub releases 页面](https://github.com/waywallen/waywallen-display/releases)。

### KDE Plasma 扩展

从最新 release 下载 `waywallen-kde-<版本>-<架构>.zip`，然后：

```sh
kpackagetool6 --type Plasma/Wallpaper -i waywallen-kde-<版本>-<架构>.zip
# -u 升级，-r 卸载
```

> [!TIP]
> KDE Plasma 用户安装/升级 QML 插件后需要重启 shell 才会被识别：
>
> ```sh
> systemctl --user restart plasma-plasmashell.service
> ```

## 扩展

| 扩展 | 说明 |
|------------|----------|
| [kde](./extensions/kde) | Plasma 6 壁纸插件 |

## 从源码构建

见 [BUILD.md](./BUILD.md)。
