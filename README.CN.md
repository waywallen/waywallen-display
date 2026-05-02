# waywallen-display

`waywallen` 壁纸守护进程的桌面集成层 —— 让 KDE Plasma、GNOME Shell 等  
Linux 桌面环境把 `waywallen` 的壁纸输出当作普通 surface 显示，通过 DMA-BUF  
零拷贝共享 GPU 资源。

## 已实现

- **协议客户端** —— C 库，与 daemon 走 `waywallen-display` v1 协议，接收
  DMA-BUF 帧 + acquire/release 同步 fence。
- **Qt 6 QML 插件**（`Waywallen.Display`）—— 在 Qt Quick 场景中即插即用的
  `WaywallenSurface` 组件。
- **KDE Plasma 壁纸扩展** —— 基于 QML 插件的 Plasma 6 kpackage。

GNOME Shell 扩展在路线图中。

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
