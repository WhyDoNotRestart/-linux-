#!/usr/bin/env bash
# 安装 sysmon 守护进程为用户级 systemd 服务
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
BIN="$HERE/build/sysmon"

[[ -x "$BIN" ]] || { echo "先 make：$HERE/build/sysmon 不存在"; exit 1; }

mkdir -p "$HOME/.local/bin" "$HOME/.config/systemd/user"
install -m 755 "$BIN" "$HOME/.local/bin/sysmon"
install -m 644 "$HERE/sysmon.service" "$HOME/.config/systemd/user/sysmon.service"

systemctl --user daemon-reload
systemctl --user enable --now sysmon.service
sleep 1
systemctl --user --no-pager status sysmon.service | head -6
echo
echo "健康检查: curl -s http://127.0.0.1:8787/healthz"
