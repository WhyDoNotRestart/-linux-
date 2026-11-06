#!/usr/bin/env bash
# 安装 sysmon GNOME 扩展（用户级），安装后需要注销并重新登录才会加载
set -euo pipefail

SRC="$(cd "$(dirname "$0")" && pwd)/sysmon@local"
DEST="$HOME/.local/share/gnome-shell/extensions/sysmon@local"

mkdir -p "$(dirname "$DEST")"
rm -rf "$DEST"
cp -r "$SRC" "$DEST"
chmod -R a+rX "$DEST"

# 写入 enabled-extensions（运行中的 shell 不会立刻重扫目录，重登后生效）
CUR=$(gsettings get org.gnome.shell enabled-extensions)
if [[ "$CUR" != *"sysmon@local"* ]]; then
  python3 - "$CUR" <<'PY'
import ast, subprocess, sys
cur = ast.literal_eval(sys.argv[1])
cur.append('sysmon@local')
subprocess.run(['gsettings', 'set', 'org.gnome.shell', 'enabled-extensions', str(cur)], check=True)
PY
fi

echo "已安装到 $DEST"
echo "请注销并重新登录（Wayland 下无法热重载 shell），顶栏右侧会出现 sysmon 指示器。"
echo "卸载：$0 --uninstall"
