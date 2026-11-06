#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$HOME/.local/bin" "$HOME/.local/share/applications"
install -m 755 "$HERE/tpkb" "$HOME/.local/bin/tpkb"
install -m 755 "$HERE/tpkb-switch" "$HOME/.local/bin/tpkb-switch"
install -m 644 "$HERE/连接 ThinkPad 蓝牙键盘.desktop" "$HOME/.local/share/applications/连接 ThinkPad 蓝牙键盘.desktop"
command -v update-desktop-database >/dev/null && update-desktop-database "$HOME/.local/share/applications" 2>/dev/null || true
echo "已安装：tpkb、tpkb-switch 到 $HOME/.local/bin"
echo "点桌面「连接 ThinkPad 蓝牙键盘」，或运行 tpkb --notify"
