#!/usr/bin/env bash
# 为当前用户启用服务与 GNOME 扩展（.deb 已安装后运行）
set -euo pipefail
systemctl --user daemon-reload
systemctl --user enable --now sysmon.service
sleep 1

EXT_SRC="/usr/share/gnome-shell/extensions/sysmon@local"
EXT_DST="$HOME/.local/share/gnome-shell/extensions/sysmon@local"
if [[ -d "$EXT_SRC" ]]; then
  mkdir -p "$(dirname "$EXT_DST")"
  rm -rf "$EXT_DST"
  cp -r "$EXT_SRC" "$EXT_DST"
  chmod -R a+rX "$EXT_DST"
  python3 - <<'PY'
import ast, subprocess
cur = ast.literal_eval(subprocess.run(['gsettings','get','org.gnome.shell','enabled-extensions'],
                                      capture_output=True, text=True).stdout)
if 'sysmon@local' not in cur:
    cur.append('sysmon@local')
    subprocess.run(['gsettings','set','org.gnome.shell','enabled-extensions',str(cur)], check=True)
PY
fi

curl -sf http://127.0.0.1:8787/healthz >/dev/null && echo "服务正常：http://127.0.0.1:8787/" || echo "服务未响应，检查 systemctl --user status sysmon"
echo "注销重登后顶栏出现 sysmon；卸载扩展：gnome-extensions disable sysmon@local"
