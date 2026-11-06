# Linux 小工具集

本仓库包含三个独立的小项目，每个都在自己的目录里，互不依赖。

| 目录 | 内容 | 形态 |
|---|---|---|
| [`sysmon/`](sysmon/) | 本机资源与设备监控：C 守护进程 + Web 面板 + TUI + GNOME 顶栏扩展 | 二进制 + 源码 + 安装包 |
| [`desktop/display-brightness/`](desktop/display-brightness/) | 外接显示器亮度调节（Gamma / DDC-CI / 内置背光） | Python + GTK4 |
| [`desktop/keyboard-connect/`](desktop/keyboard-connect/) | 一键连接 / 重新配对 ThinkPad 蓝牙键盘 | Python + Bash |

## 环境

开发与验证环境：Ubuntu 26.04、GNOME Shell 50.1、Wayland。

各项目依赖不同，见各自目录下的 README。

## 快速开始

```bash
# 1. 系统监控（系统级安装）
sudo dpkg -i sysmon/dist/sysmon_1.0.0_amd64.deb
sysmon-setup            # 启用用户服务 + GNOME 扩展（之后注销重登）

# 2. 显示器亮度
cd desktop/display-brightness && ./install.sh

# 3. 蓝牙键盘
cd desktop/keyboard-connect && ./install.sh
```

## 目录约定

每个项目目录都包含：说明文档（README）、可执行文件或源码、以及一键安装脚本（`install.sh`）。卸载方式写在各自的 README 里。
