#!/usr/bin/env bash
# 免 root 安装：二进制到 ~/.local/bin，服务与扩展装到用户目录
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
BIN="$HOME/.local/bin"
UNIT="$HOME/.config/systemd/user"
EXT="$HOME/.local/share/gnome-shell/extensions/sysmon@local"

mkdir -p "$BIN" "$UNIT" "$(dirname "$EXT")"
install -m 755 "$HERE/bin/sysmon"     "$BIN/sysmon"
install -m 755 "$HERE/bin/sysmon-tui" "$BIN/sysmon-tui"
install -m 644 "$HERE/systemd/sysmon.service" "$UNIT/sysmon.service"

rm -rf "$EXT"
cp -r "$HERE/gnome/sysmon@local" "$EXT"
chmod -R a+rX "$EXT"

systemctl --user daemon-reload
systemctl --user enable --now sysmon.service
sleep 1

python3 - <<'PY'
import ast, subprocess
cur = ast.literal_eval(subprocess.run(['gsettings','get','org.gnome.shell','enabled-extensions'],
                                      capture_output=True, text=True).stdout)
if 'sysmon@local' not in cur:
    cur.append('sysmon@local')
    subprocess.run(['gsettings','set','org.gnome.shell','enabled-extensions',str(cur)], check=True)
PY

echo "已安装到 $BIN/sysmon、$BIN/sysmon-tui"
curl -sf http://127.0.0.1:8787/healthz >/dev/null && echo "服务已启动：http://127.0.0.1:8787/" || echo "服务未响应，检查 systemctl --user status sysmon"
echo "注销重登后顶栏出现 sysmon。TUI 直接运行 sysmon-tui"
