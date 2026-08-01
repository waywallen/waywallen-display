# waywallen-display

<a href="README.md">English README</a> · <a href="README.CN.md">中文 README</a>

Интеграция фоновой службы обоев `waywallen` с рабочим столом. Позволяет KDE Plasma,
GNOME Shell и другим оболочкам Linux отображать вывод `waywallen` как обычную
поверхность с передачей изображения без копирования через DMA-BUF.

## Что реализовано

- **Клиент протокола** — библиотека C, которая использует протокол
  `waywallen-display` v1 и получает кадры DMA-BUF с сигналами синхронизации
  acquire/release.
- **Модуль Qt 6 QML** (`Waywallen.Display`) — готовый компонент
  `WaywallenSurface` для сцен Qt Quick.
- **Модуль GObject** — вспомогательный модуль для создания `GdkTexture` через
  `waywallen-display`.
- **Расширение обоев KDE Plasma** — пакет Plasma 6 на основе модуля QML.
- **Расширение GNOME Shell** — расширение Shell 48+ на основе модуля GObject.
- **Клиент Wayland layer-shell** — отдельный клиент обоев для композиторов,
  предоставляющих `zwlr_layer_shell_v1`.

## Установка

Готовые сборки публикуются на странице
[GitHub Releases](https://github.com/waywallen/waywallen-display/releases).

### Расширение KDE Plasma

Загрузите `waywallen-kde-<версия>-<архитектура>-embed.zip` из последнего выпуска:

```sh
kpackagetool6 --type Plasma/Wallpaper -i waywallen-kde-<версия>-<архитектура>-embed.zip
# Для обновления используйте -u, для удаления — -r
```

> [!TIP]
> После установки или обновления модуля QML перезапустите оболочку Plasma:
>
> ```sh
> systemctl --user restart plasma-plasmashell.service
> ```

> [!TIP]
> В сеансах X11, например на Steam Deck, plasmashell требуется бэкенд EGL XCB
> для импорта DMA-BUF обоев. Создайте пользовательский drop-in systemd
> `~/.config/systemd/user/plasma-plasmashell.service.d/override.conf`:
>
> ```ini
> [Service]
> Environment=QT_XCB_GL_INTEGRATION=xcb_egl
> ```
>
> Затем перечитайте конфигурацию и перезапустите Plasma:
>
> ```sh
> systemctl --user daemon-reload
> systemctl --user restart plasma-plasmashell.service
> ```

### Расширение GNOME Shell

Для GNOME Shell в сеансе Wayland (проверено на Shell 50) загрузите
`waywallen-gnome-<версия>-<архитектура>.zip` из последнего выпуска:

```sh
gnome-extensions install --force waywallen-gnome-<версия>-<архитектура>.zip
gnome-extensions enable org.waywallen.gnome@waywallen.io
```

Выйдите из сеанса и войдите снова, чтобы загрузить расширение.

### Клиент Wayland layer-shell

Для композиторов Wayland, предоставляющих `zwlr_layer_shell_v1`, например
Hyprland, Sway и Niri, загрузите
`waywallen-layer-shell-<версия>-<архитектура>.tar.gz` из последнего выпуска:

```sh
tar -xzf waywallen-layer-shell-<версия>-<архитектура>.tar.gz
install -Dm755 waywallen-layer-shell-<версия>-<архитектура>/waywallen-layer-shell ~/.local/bin/waywallen-layer-shell
cp -r waywallen-layer-shell-<версия>-<архитектура>/share ~/.local/
```

Каталог `share` содержит файлы переводов. Сохраните его при переносе исполняемого
файла, если требуется локализованный вывод CLI.

```sh
waywallen-layer-shell
# Необязательно:
waywallen-layer-shell --socket "$XDG_RUNTIME_DIR/waywallen/display.sock"
```

> [!NOTE]
> Версия `waywallen` в формате AppImage уже содержит `waywallen-layer-shell` и
> самостоятельно управляет им.

## Расширения

| Расширение | Описание |
|------------|----------|
| [kde](./extensions/kde) | Модуль обоев Plasma 6 |
| [gnome](./extensions/gnome) | Расширение GNOME Shell 48–50 для Wayland |
| layer-shell | Клиент Wayland `zwlr_layer_shell_v1` |

## Сборка из исходного кода

См. [BUILD.md](./BUILD.md).
