#!/usr/bin/env bash
# 冒烟测试：采集、接口、应用分组、结束应用
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
PORT=8899
BIN="$HERE/build/sysmon"
PASS=0; FAIL=0

ok()   { echo "  ✔ $1"; PASS=$((PASS+1)); }
bad()  { echo "  ✘ $1"; FAIL=$((FAIL+1)); }

[[ -x "$BIN" ]] || { echo "先 make"; exit 1; }

"$BIN" --port $PORT >/tmp/sysmon_smoke.log 2>&1 &
SRV=$!
sleep 2

curl -sf "http://127.0.0.1:$PORT/healthz" | grep -q '"ok":true' && ok "healthz" || bad "healthz"

SNAP=$(curl -sf "http://127.0.0.1:$PORT/api/v1/snapshot")
echo "$SNAP" | python3 -c 'import json,sys; d=json.load(sys.stdin); assert d["mem"]["total"]>0 and len(d["cpu"]["cores"])>0 and len(d["net"])>=0' \
  && ok "snapshot 结构" || bad "snapshot 结构"

curl -sf "http://127.0.0.1:$PORT/api/v1/apps" | python3 -c 'import json,sys; d=json.load(sys.stdin); assert d["total"]>=0' \
  && ok "apps 接口" || bad "apps 接口"

# 应用分组 + 结束
bash -c 'exec -a smoketest /bin/sleep 60' &
T=$!
sleep 6
KEY=$(curl -sf "http://127.0.0.1:$PORT/api/v1/apps" | python3 -c '
import json,sys
d=json.load(sys.stdin)
for a in d["apps"]:
    if "smoketest" in a["app"].lower():
        print(a["key"]); break
')
if [[ -n "$KEY" ]]; then
  ok "应用分组找到 smoketest"
  R=$(curl -sf -X POST -H 'Content-Type: application/json' -d "{\"key\":\"$KEY\"}" "http://127.0.0.1:$PORT/api/v1/kill-app")
  echo "$R" | grep -q '"ok":true' && ok "kill-app 返回 ok" || bad "kill-app 返回 $R"
  sleep 1
  kill -0 $T 2>/dev/null && bad "进程仍存活" || ok "进程已被结束"
else
  bad "应用分组没找到 smoketest"
fi

kill $SRV 2>/dev/null
echo
echo "结果：$PASS 通过，$FAIL 失败"
[[ $FAIL -eq 0 ]]
