# GNOME Shell 扩展（P1：顶栏指示器 + 下拉看板）

这是第一阶段的桌面集成：顶栏显示 `CPU% · 内存%`，点开是一个小看板。

## 安装

```bash
cd Linux系统监控面板/脚本/gnome
./install.sh
```

脚本会做三件事：

1. 复制 `sysmon@local/` 到 `~/.local/share/gnome-shell/extensions/`
2. 修正文件权限（`644/755`）
3. 把 `sysmon@local` 追加进 `org.gnome.shell enabled-extensions`

**Wayland 下无法热重载 shell，必须注销再登录才会加载。**

改了 `extension.js` 或 `stylesheet.css` 之后，同样要重新复制并重登（GNOME 不会重新 import 已加载的模块）。

> 本机 GNOME 50 的 `org.gnome.Shell.Extensions.ReloadExtension` 虽然出现在 introspection 里，实际调用返回 `UnknownMethod`，所以目前只能靠重登刷新扩展代码。

## 使用

登录后：

- 顶栏右侧出现一个显示器小图标 + `23% · 62%`（CPU · 内存）
- 点击展开看板（2×2 指标卡 + 进度条 + 进程列表，见 `../../素材与样例/顶栏菜单_设计稿_2026-11-06.png`）：
  - 主机名与运行时长
  - CPU 占用与 load
  - 内存 已用 / 总量
  - 网络 ↓/↑ 速率
  - 最高温度
  - 电池
  - 占用最高的 3 个进程
  - 「打开完整面板」→ 浏览器打开 http://127.0.0.1:8787/
  - 「刷新」→ 立即拉一次
- 每 2 秒自动刷新；守护进程没跑时显示「离线」

## 前置条件

扩展只负责显示，数据来自本机守护进程：

```bash
./build/sysmon --port 8787        # 建议做成 systemd user 服务
```

## 卸载

```bash
./uninstall.sh        # 移除目录并摘掉 enabled-extensions 里的条目
```

## 故障排查

| 现象 | 处理 |
|---|---|
| 顶栏没有图标 | 确认已注销重登；`gsettings get org.gnome.shell enabled-extensions` 里应有 `sysmon@local` |
| 显示「离线」 | `curl 127.0.0.1:8787/healthz`，守护进程没起 |
| 扩展报错 | `journalctl --user -b -o cat | grep -i sysmon`，或在「Looking Glass」里跑 `Main.extensionManager.lookup('sysmon@local')` |
| 想临时停用 | `gnome-extensions disable sysmon@local`（仍需重登生效） |

## 已知限制

- 只在 GNOME 50 上验证过 API 形态（PanelMenu / PopupMenu / Soup 3）；更低版本未测。
- 轮询式（2 s 一次 HTTP），没有订阅 SSE；进程多时也只会拿 Top 12。
- 系统代理会让回环请求走代理，代码里已对 session 关闭 proxy resolver。
- 桌面挂件（背景层 + 鼠标穿透）是下一阶段，不在本目录。