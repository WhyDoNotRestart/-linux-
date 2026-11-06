# sysmon 核心（C 守护进程 + Web 前端）

单二进制、无第三方依赖（只 libc + pthread），采集 /proc 与 /sys，提供 HTTP/SSE，并支持按占用结束进程。

## 安装为开机服务

```bash
./install-service.sh
systemctl --user status sysmon        # active (running)
curl -s 127.0.0.1:8787/healthz
```

对应 `sysmon.service`：`Restart=on-failure`、`MemoryMax=64M`、`NoNewPrivileges=yes`，装到 `~/.config/systemd/user/`，二进制装到 `~/.local/bin/sysmon`。

## 构建

```bash
make            # -> build/sysmon
make clean
```

前端 `web/index.html` 由 `xxd -i` 编进二进制（`build/assets.h`），改完前端重新 `make` 即可。

## 两个前端

| 二进制 | 形态 | 常驻 | 说明 |
|---|---|---|---|
| `build/sysmon` | Web | 2.6 MB | 浏览器面板 + kill 按钮 |
| `build/sysmon-tui` | 终端 | **3.5 MB** | ncurses，1 线程，无需浏览器 |

### Web 多页

左侧图标栏对应四个页面，点一下就切：

| 页 | 内容 |
|---|---|
| 总览 | KPI、CPU/网络曲线、内存环形、磁盘/电池、进程表、设备/传感器 |
| 内存 | 11 项 meminfo 明细 + PSI some/full |
| 系统 | 主机/发行版/内核/CPU/线程/运行时长；根分区/磁盘读写/电池/网卡/传感器数 |
| 设备 | USB 列表（含 VID:PID、bus）、显示输出状态、PCI 数量、网卡、电池 |

也支持直接开某一页：`http://127.0.0.1:8787/#mem`、`#sys`、`#dev`。

### 应用视图（类 Windows 任务管理器）

进程卡默认按**应用**聚合：

- 分组依据：进程的 cgroup scope（systemd 的 `app-*` / `snap.*`），没有 scope 时按应用名合并
- 显示名：先查 `.desktop` 的 `Name=`（`/usr/share/applications` + 用户目录），再退回内置别名（Microsoft Edge、Visual Studio Code、GNOME Shell……），最后才用可执行名
- systemd 会把 desktop id 里的 `-` 转义成 `\x2d`，代码里已还原
- 一行显示：应用名 / 进程数 / 合计 CPU / 合计内存 / **结束**
- 「结束」会向该应用**所有进程**发 SIGTERM；服务端同样校验 uid、PID 1、自身与祖先进程，并写审计日志（`kill-app`）
- 顶部可切「应用 / 进程」两种视图，过滤框对两者都生效

### Web 进程过滤

进程卡右上角有过滤框：输入即筛（220 ms 防抖），匹配**应用名 / 完整命令行 / 用户**，计数显示 `匹配 16 / 64`。快捷键：`/` 聚焦，`Esc` 清空。也支持直接开链接：

```
http://127.0.0.1:8787/?q=kworker
```

过滤在服务端执行（`q=`），所以进程多的时候也只传回命中的那些；表格区最高 320 px，超出滚动。

### TUI

```bash
./build/sysmon-tui                  # 进入界面（默认总览）
./build/sysmon-tui --tab 2          # 直接打开进程页
./build/sysmon-tui --filter firefox # 打开时就带上过滤词
./build/sysmon-tui --frames 3       # 画 3 帧后退出（截图/自测用）
```

四个标签页：

| Tab | 内容 |
|---|---|
| 1 总览 | CPU 总条 + 每核刻度、内存、网络、温度、磁盘、占用最高的进程 |
| 2 进程 | 全屏进程表：PID / 用户 / CPU% / 内存 / **应用名** / **完整命令行**，底部显示 ppid、状态与命令行 |
| 3 应用 | 按应用聚合：显示名 / 进程数 / 合计 CPU / 合计内存，`x` 结束整组 |
| 4 设备 | USB、显示输出、PCI 数量、网卡、电池、传感器、交换 |
| 5 系统 | 主机、发行版、内核、CPU 型号、线程、运行时长、内存/交换/根分区、PSI |

| 键 | 作用 |
|---|---|
| `1`–`5` / `Tab` / `Shift-Tab` | 切换标签页 |
| `q` | 退出 |
| `/` | 进入过滤输入（匹配应用名、命令行、用户；Enter 应用，Esc 清空，退格删除） |
| `s` | 切换排序（内存 / CPU，进程页） |
| `↑` `↓` | 选择进程（进程页） |
| `x` / `X` | 结束进程（进程页）或结束整个应用（应用页），带确认框 |
| `p` | 暂停刷新 |

过滤是大小写不敏感的，进程页标题会显示 `匹配 2 / 64`，底栏右侧显示 `[sys]`。

进程的「应用名」取 `argv[0]` 的 basename（如 `sysmon-tui`、`bash`、`firefox`），右侧「命令」是完整 `cmdline`（控制字符已清洗、连续空格已折叠），所以能直接看出是哪个应用、带什么参数。

TUI 与 Web 共用 `collect.c` 和 `audit.c`，kill 同样写审计日志（`peer=tui`）。

## 运行

```bash
./build/sysmon --port 8787                   # 默认 127.0.0.1:8787，1 秒刷新
./build/sysmon --proc-interval 3000          # 进程扫描间隔（默认 3000 ms）
./build/sysmon --log ~/kill.log              # 指定审计日志（默认 ~/.local/state/sysmon/kill.log）
./build/sysmon --once                        # 打印一份快照 JSON 后退出
```

进程扫描与设备扫描是**降频**的：主循环 1 Hz 采内存/CPU/网络，进程表每 3 秒刷新一次，USB/显示/PCI 每 10 秒枚举一次，温度路径每 30 秒重新发现一次。中间帧复用缓存，所以界面仍是 1 秒一跳。

浏览器打开 http://127.0.0.1:8787/ ；`?static=1` 只拉一次数据，不建 SSE 连接（截图/低功耗用）。

## 接口

| 路径 | 说明 |
|---|---|
| `GET /` | 内嵌前端 |
| `GET /api/v1/snapshot` | 当前快照（内存/CPU/网络/磁盘/温度/电池/设备/进程 Top12） |
| `GET /api/v1/history` | 最近 120 个采样点 |
| `GET /api/v1/procs?sort=rss|cpu&limit=N&q=词` | 进程列表（含 user / CPU% / RSS / cmd / scope）；`q` 过滤名称、命令行、用户 |
| `GET /api/v1/apps` | 按应用分组：display（桌面名）、count、cpu、rss、pids |
| `POST /api/v1/kill-app` | `{"key":"app:firefox"}` 或 `{"app":"firefox"}` 或 `{"pid":N}`，结束整个应用 |
| `GET /api/v1/stream` | SSE，每秒一帧 `tick` |
| `POST /api/v1/kill` | `{"pid":123,"signal":"TERM"}`，返回 `{"ok":true}` |
| `GET /healthz` | 自身 RSS / 线程数 / CPU 占用 / 审计日志路径 / 累计请求数 |

### 结束进程的安全规则

守护进程会拒绝以下请求（返回 400 + 原因）：

1. PID ≤ 1（init）
2. sysmon 自身、父进程、任何祖先进程
3. 非当前用户拥有的进程（root 或其他用户）
4. 未列出的信号（只允许 TERM / KILL / INT / HUP）

信号默认 SIGTERM；前端可勾选「强制结束」改发 SIGKILL。所有拒绝都在服务端判断，前端不是唯一防线。

> 注意：`POST` 需要 `Content-Type: application/json`。跨站表单无法发出该类型，因此本机端口不会被网页 CSRF 直接调用。

## 安装包

```bash
./packaging/build-packages.sh
```

产出在 `dist/`：

| 文件 | 用途 |
|---|---|
| `sysmon_1.0.0_amd64.deb` | Ubuntu/Debian 系统级安装（`sudo dpkg -i`），装完运行 `sysmon-setup` 启用当前用户的服务与扩展 |
| `sysmon-1.0.0-linux-amd64.tar.gz` | 免 root：解压后 `./install.sh`，二进制进 `~/.local/bin`，服务与扩展进用户目录 |

卸载：deb 用 `sudo dpkg -r sysmon`（再 `gnome-extensions disable sysmon@local`）；tarball 用 `./uninstall.sh`。

## 冒烟测试

```bash
./test_smoke.sh
```

覆盖 healthz、snapshot 结构、apps 接口、应用分组、kill-app 真结束进程，6 项断言。

## 审计日志

每次 kill（成功、失败、被拒）都会写一行：

```
2026-11-05T21:35:34+0800 kill ok pid=7 uid=1000 name=sleep signal=TERM peer=127.0.0.1
2026-11-05T21:31:58+0800 kill refused pid=1 reason=refused: pid 1
```

- 默认路径：`$XDG_STATE_HOME/sysmon/kill.log`，未设置则 `~/.local/state/sysmon/kill.log`
- 目录不存在会自动创建；目录不可写则退回 stderr（`/healthz` 的 `audit` 字段会显示实际路径）
- 每行立即 `fflush`，断电不丢最后一条
- 目前不做轮转，可用 `logrotate` 或定期清理

## 实测（本机，2026-11-05）

| 指标 | 值 |
|---|---|
| 二进制 | Web 89 KB / TUI 49 KB |
| 常驻 RSS | Web 2.58 MB / TUI 3.5 MB |
| 自身 CPU（稳定态，60 s 窗口） | **0.31%** |
| 快照 JSON | 约 2 KB |
| 首字节 | < 1 ms |

降频前是 0.53–0.57%，主要开销不在进程扫描，而在每秒重扫 hwmon 与设备树；改成「路径缓存 + 设备 10 s」后才降下来。

## 已知限制

- 进程列表只覆盖当前 PID 命名空间；在沙箱里跑只能看到沙箱内进程，正常桌面会话下是完整列表。
- 设备页当前含 USB（过滤 root hub）、显示输出、PCI 计数；蓝牙与摄像头型号尚未单独归类。
- 温度只取 hwmon；风扇转速未接。
- 审计日志不轮转、不压缩。

## 目录

```
src/state.h    快照结构
src/collect.c  /proc /sys 采集 + 进程扫描
src/json.c     JSON 序列化
src/http.c     极简 HTTP + SSE + kill 接口
src/main.c     采样循环
web/index.html 前端（内嵌）
```