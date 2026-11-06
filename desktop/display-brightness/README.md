# 外接显示器亮度调节

统一调节显示器亮度，覆盖三种情况：

1. **软件 Gamma**：通过 GNOME Mutter DisplayConfig（Wayland）或 xrandr（X11）缩放 Gamma LUT，任何外接屏都能用，包括不支持 DDC/CI 的型号
2. **硬件 DDC/CI**：通过 `ddcutil` 直接调显示器背光
3. **笔记本内置屏**：通过 Mutter Backlight / sysfs

界面是 GTK4 单窗口，支持多显示器分别调节、记住上次设置。

## 依赖

```bash
sudo apt install python3-gi gir1.2-gtk-4.0
# 可选：硬件 DDC/CI
sudo apt install ddcutil
# 可选：DBus 控制
sudo apt install python3-dbus
```

## 安装

```bash
./install.sh
```

会复制到：

- `~/.local/bin/external-brightness-gui`
- `~/.local/share/applications/外接显示器亮度.desktop`

之后从应用菜单启动「显示器亮度调节」，或把 desktop 文件拖到桌面。

## 手动运行

```bash
external-brightness-gui
```

## 文件

| 文件 | 说明 |
|---|---|
| `external-brightness-gui` | 主程序（Python3 + GTK4） |
| `外接显示器亮度.desktop` | 桌面/菜单启动器 |
| `install.sh` | 安装脚本 |

## 限制

- DDC/CI 需要显示器硬件支持，且 i2c 设备可能需要把用户加入 `i2c` 组
- Wayland 下走 Mutter 的 Gamma 接口，部分合成器版本可能不开放
- 配置文件在 `~/.config/external-brightness/config.json`
