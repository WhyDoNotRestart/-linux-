#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$HOME/.local/bin" "$HOME/.local/share/applications"
install -m 755 "$HERE/external-brightness-gui" "$HOME/.local/bin/external-brightness-gui"
install -m 644 "$HERE/外接显示器亮度.desktop" "$HOME/.local/share/applications/外接显示器亮度.desktop"
command -v update-desktop-database >/dev/null && update-desktop-database "$HOME/.local/share/applications" 2>/dev/null || true
echo "已安装：$HOME/.local/bin/external-brightness-gui"
echo "从应用菜单启动「显示器亮度调节」，或运行 external-brightness-gui"
