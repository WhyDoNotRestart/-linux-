#!/usr/bin/env bash
set -euo pipefail
DEST="$HOME/.local/share/gnome-shell/extensions/sysmon@local"
rm -rf "$DEST"
python3 - <<'PY'
import ast, subprocess
cur = ast.literal_eval(subprocess.run(['gsettings','get','org.gnome.shell','enabled-extensions'],
                                      capture_output=True, text=True).stdout)
if 'sysmon@local' in cur:
    cur.remove('sysmon@local')
    subprocess.run(['gsettings','set','org.gnome.shell','enabled-extensions',str(cur)], check=True)
PY
echo "已卸载，注销后生效。"
