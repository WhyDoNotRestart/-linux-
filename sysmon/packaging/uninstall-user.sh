#!/usr/bin/env bash
set -euo pipefail
systemctl --user disable --now sysmon.service 2>/dev/null || true
rm -f "$HOME/.config/systemd/user/sysmon.service"
systemctl --user daemon-reload || true
rm -rf "$HOME/.local/share/gnome-shell/extensions/sysmon@local"
rm -f "$HOME/.local/bin/sysmon" "$HOME/.local/bin/sysmon-tui"
python3 - <<'PY'
import ast, subprocess
cur = ast.literal_eval(subprocess.run(['gsettings','get','org.gnome.shell','enabled-extensions'],
                                      capture_output=True, text=True).stdout)
if 'sysmon@local' in cur:
    cur.remove('sysmon@local')
    subprocess.run(['gsettings','set','org.gnome.shell','enabled-extensions',str(cur)], check=True)
PY
echo "已卸载（注销后顶栏图标消失）"
