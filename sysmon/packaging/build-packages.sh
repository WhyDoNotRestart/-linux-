#!/usr/bin/env bash
# 构建 .deb 与免 root tarball
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
OUT="$ROOT/dist"
VER="1.0.0"
ARCH="amd64"

rm -rf "$OUT/build"
mkdir -p "$OUT"

DEB="$OUT/build/sysmon_${VER}_${ARCH}"
mkdir -p "$DEB/DEBIAN" "$DEB/usr/bin" "$DEB/usr/lib/systemd/user" \
         "$DEB/usr/share/gnome-shell/extensions/sysmon@local" \
         "$DEB/usr/share/doc/sysmon" "$DEB/usr/share/applications"

install -m 755 "$ROOT/build/sysmon"     "$DEB/usr/bin/sysmon"
install -m 755 "$ROOT/build/sysmon-tui" "$DEB/usr/bin/sysmon-tui"
strip --strip-unneeded "$DEB/usr/bin/sysmon" "$DEB/usr/bin/sysmon-tui"

cat > "$DEB/usr/lib/systemd/user/sysmon.service" <<'UNIT'
[Unit]
Description=sysmon 系统监控守护进程（127.0.0.1:8787）
After=graphical-session.target

[Service]
Type=simple
ExecStart=/usr/bin/sysmon --port 8787
Restart=on-failure
RestartSec=2
MemoryMax=64M
NoNewPrivileges=yes

[Install]
WantedBy=default.target
UNIT

cp "$ROOT/gnome/sysmon@local/"* "$DEB/usr/share/gnome-shell/extensions/sysmon@local/"
install -m 755 "$HERE/sysmon-setup.sh" "$DEB/usr/bin/sysmon-setup"
install -m 644 "$HERE/sysmon.desktop"  "$DEB/usr/share/applications/sysmon.desktop"
install -m 644 "$ROOT/README.md"       "$DEB/usr/share/doc/sysmon/README.md"

SIZE=$(du -sk "$DEB" | cut -f1)
cat > "$DEB/DEBIAN/control" <<CTRL
Package: sysmon
Version: ${VER}
Section: utils
Priority: optional
Architecture: ${ARCH}
Installed-Size: ${SIZE}
Depends: libc6 (>= 2.34), libncursesw6, libtinfo6
Maintainer: local <local@localhost>
Description: 本机 Linux 资源与设备监控面板
 单二进制守护进程 + Web 面板 + TUI + GNOME 顶栏扩展。
 采集 /proc 与 /sys：CPU、内存、网络、磁盘、温度、电池、USB、
 显示输出，并支持按应用结束进程（带审计日志）。
CTRL

cat > "$DEB/DEBIAN/postinst" <<'POST'
#!/bin/sh
set -e
echo ""
echo "sysmon 已安装。下一步（用你的普通用户执行）："
echo "  sysmon-setup      # 启用用户服务 + 安装 GNOME 扩展"
echo "  然后注销重登，顶栏会出现 sysmon 指示器"
echo ""
POST
find "$DEB" -type d -exec chmod 755 {} +
find "$DEB" -type f -exec chmod 644 {} +
chmod 755 "$DEB/usr/bin/sysmon" "$DEB/usr/bin/sysmon-tui" "$DEB/usr/bin/sysmon-setup" "$DEB/DEBIAN/postinst"

dpkg-deb --root-owner-group --build "$DEB" "$OUT/sysmon_${VER}_${ARCH}.deb" >/dev/null

TAR="$OUT/build/sysmon-${VER}-linux-${ARCH}"
mkdir -p "$TAR/bin" "$TAR/systemd" "$TAR/gnome"
install -m 755 "$ROOT/build/sysmon"     "$TAR/bin/sysmon"
install -m 755 "$ROOT/build/sysmon-tui" "$TAR/bin/sysmon-tui"
strip --strip-unneeded "$TAR/bin/sysmon" "$TAR/bin/sysmon-tui"
cp -r "$ROOT/gnome/sysmon@local" "$TAR/gnome/"
install -m 644 "$ROOT/sysmon.service" "$TAR/systemd/sysmon.service"
install -m 644 "$ROOT/README.md" "$TAR/README.md"
install -m 755 "$HERE/install-user.sh"   "$TAR/install.sh"
install -m 755 "$HERE/uninstall-user.sh" "$TAR/uninstall.sh"

find "$TAR" -type d -exec chmod 755 {} +
find "$TAR" -type f -exec chmod 644 {} +
chmod 755 "$TAR/bin/sysmon" "$TAR/bin/sysmon-tui" "$TAR/install.sh" "$TAR/uninstall.sh"

tar -C "$OUT/build" -czf "$OUT/sysmon-${VER}-linux-${ARCH}.tar.gz" "sysmon-${VER}-linux-${ARCH}"
echo "产出："
ls -lh "$OUT"/sysmon_*.deb "$OUT"/sysmon-*.tar.gz
