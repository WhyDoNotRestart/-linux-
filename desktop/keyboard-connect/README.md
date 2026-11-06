# 连接 ThinkPad 蓝牙键盘

针对 ThinkPad Compact Bluetooth Keyboard 的一键连接工具，解决两个常见问题：

- 电脑休眠 / 切换设备后，键盘显示已配对但连不上
- 需要重新配对时，蓝牙配对码（Passkey）输入流程麻烦

## 两个脚本

| 脚本 | 作用 |
|---|---|
| `tpkb` | 快速连接：解除 rfkill、开机蓝牙、`bluetoothctl connect`，并校验输入设备真的注册了 |
| `tpkb-switch` | 修复式连接：连接失败时走重新配对流程，自动处理 Passkey，适合「从别的设备切回来」 |

## 依赖

```bash
sudo apt install bluez python3-pexpect
# 可选：桌面通知
sudo apt install libnotify-bin
```

## 安装

```bash
./install.sh
```

复制到 `~/.local/bin/` 与 `~/.local/share/applications/`，之后点桌面图标「连接 ThinkPad 蓝牙键盘」。

## 使用

```bash
tpkb              # 只连接
tpkb --notify     # 连接并弹桌面通知
tpkb-switch       # 连接失败时重新配对（会弹 Passkey 提示）
```

## 换成你自己的键盘

两个脚本里都硬编码了地址 `90:7F:61:11:01:29`，换键盘后改这两处：

```bash
bluetoothctl devices        # 查你的设备地址
# 修改 tpkb 与 tpkb-switch 里的 ADDRESS / address 变量
```

## 文件

| 文件 | 说明 |
|---|---|
| `tpkb` | 快速连接（Bash） |
| `tpkb-switch` | 重新配对（Python + pexpect） |
| `连接 ThinkPad 蓝牙键盘.desktop` | 桌面启动器 |
| `install.sh` | 安装脚本 |

## 说明

- 需要 `bluetoothctl`（bluez）在 PATH 中
- 连接是否成功不只看 `Connected: yes`，还会检查 `/proc/bus/input/devices` 里是否真的出现键盘，避免「假连接」
