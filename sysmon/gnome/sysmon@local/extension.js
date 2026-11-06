import Clutter from 'gi://Clutter';
import GLib from 'gi://GLib';
import GObject from 'gi://GObject';
import Soup from 'gi://Soup';
import St from 'gi://St';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as PanelMenu from 'resource:///org/gnome/shell/ui/panelMenu.js';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

const API = 'http://127.0.0.1:8787/api/v1/snapshot';
const REFRESH_SEC = 2;
const BOARD_W = 360;
const TILE_W = 163;
const BAR_W = 143;
const GB = 1073741824;

function fmtRate(bps) {
    if (bps >= 1048576) return (bps / 1048576).toFixed(1) + ' MB/s';
    if (bps >= 1024) return (bps / 1024).toFixed(0) + ' KB/s';
    return Math.round(bps) + ' B/s';
}

function fmtGB(bytes) {
    return (bytes / GB).toFixed(1);
}

const SysmonIndicator = GObject.registerClass(
class SysmonIndicator extends PanelMenu.Button {
    _init() {
        super._init(0.0, 'sysmon', false);

        this._destroyed = false;
        this._session = new Soup.Session({timeout: 2});
        try {
            this._session.set_proxy_resolver(null);
        } catch (e) {
            console.warn('sysmon: ' + e);
        }

        const box = new St.BoxLayout({style_class: 'panel-status-menu-box'});
        box.add_child(new St.Icon({
            icon_name: 'utilities-system-monitor-symbolic',
            style_class: 'system-status-icon',
        }));
        this._label = new St.Label({
            text: '--',
            y_align: Clutter.ActorAlign.CENTER,
            style_class: 'sysmon-panel-label',
        });
        box.add_child(this._label);
        this.add_child(box);

        this._rows = {};
        this._tiles = {};
        this._buildBoard();

        this._timer = GLib.timeout_add_seconds(GLib.PRIORITY_DEFAULT, REFRESH_SEC, () => {
            this._refresh();
            return GLib.SOURCE_CONTINUE;
        });
        this._refresh();
    }

    _mkLabel(text, cls) {
        return new St.Label({text, style_class: cls, y_align: Clutter.ActorAlign.CENTER});
    }

    _tile(parent, key, title, cls) {
        const tile = new St.BoxLayout({
            vertical: true,
            style_class: 'sysmon-tile ' + cls,
            width: TILE_W,
        });
        tile.add_child(this._mkLabel(title, 'sysmon-k'));
        const val = this._mkLabel('--', 'sysmon-v');
        const sub = this._mkLabel('--', 'sysmon-s');
        const bar = new St.Widget({style_class: 'sysmon-bar', width: BAR_W, height: 3});
        const fill = new St.Widget({style_class: 'sysmon-fill', width: 0, height: 3});
        bar.add_child(fill);

        tile.add_child(val);
        tile.add_child(sub);
        tile.add_child(bar);
        parent.add_child(tile);

        this._tiles[key] = {val, sub, fill};
        return this._tiles[key];
    }

    _buildBoard() {
        this.menu.box.set_style('padding: 0px;');

        const board = new St.BoxLayout({vertical: true, style_class: 'sysmon-board', width: BOARD_W});
        this.menu.box.add_child(board);
        this._board = board;

        // header
        const head = new St.BoxLayout({style_class: 'sysmon-head', x_expand: true});
        const left = new St.BoxLayout({vertical: true, x_expand: true});
        const brand = new St.BoxLayout({style_class: 'sysmon-brand'});
        brand.add_child(new St.Widget({style_class: 'sysmon-dot', width: 7, height: 7}));
        brand.add_child(this._mkLabel('sysmon', 'sysmon-name'));
        left.add_child(brand);
        this._host = this._mkLabel('连接中…', 'sysmon-host');
        left.add_child(this._host);
        head.add_child(left);
        const live = new St.BoxLayout({style_class: 'sysmon-live'});
        live.add_child(this._mkLabel('LIVE 2s', 'sysmon-live-t'));
        head.add_child(live);
        board.add_child(head);

        // 2x2 tiles
        const grid = new St.BoxLayout({vertical: true, style_class: 'sysmon-grid'});
        const r1 = new St.BoxLayout({style_class: 'sysmon-grow'});
        const r2 = new St.BoxLayout({style_class: 'sysmon-grow'});
        this._tile(r1, 'cpu', 'CPU', 'is-cpu');
        this._tile(r1, 'mem', '内存', 'is-mem');
        this._tile(r2, 'net', '网络', 'is-net');
        this._tile(r2, 'tmp', '温度', 'is-tmp');
        grid.add_child(r1);
        grid.add_child(r2);
        board.add_child(grid);

        // processes
        board.add_child(this._mkLabel('占用最高', 'sysmon-sect-t'));
        this._procRows = [];
        for (let i = 0; i < 3; i++) {
            const row = new St.BoxLayout({style_class: 'sysmon-prow', x_expand: true});
            const ico = new St.Label({text: '--', style_class: 'sysmon-pico', y_align: Clutter.ActorAlign.CENTER});
            const mid = new St.BoxLayout({vertical: true, x_expand: true});
            const nm = this._mkLabel('--', 'sysmon-pn');
            const sb = this._mkLabel('--', 'sysmon-psub');
            mid.add_child(nm);
            mid.add_child(sb);
            const cpu = this._mkLabel('--', 'sysmon-pv is-good');
            const mem = this._mkLabel('--', 'sysmon-pv is-dim');
            row.add_child(ico);
            row.add_child(mid);
            row.add_child(cpu);
            row.add_child(mem);
            board.add_child(row);
            this._procRows.push({row, ico, nm, sb, cpu, mem});
        }

        // actions
        const actions = new St.BoxLayout({style_class: 'sysmon-actions', x_expand: true});
        const open = new St.Button({style_class: 'sysmon-btn is-primary', x_expand: true});
        open.set_child(new St.Label({text: '打开完整面板', style_class: 'sysmon-btn-t'}));
        open.connect('clicked', () => {
            this.menu.close();
            GLib.spawn_command_line_async('xdg-open http://127.0.0.1:8787/');
        });
        const refresh = new St.Button({style_class: 'sysmon-btn', x_expand: true});
        refresh.set_child(new St.Label({text: '刷新', style_class: 'sysmon-btn-t'}));
        refresh.connect('clicked', () => this._refresh());
        actions.add_child(open);
        actions.add_child(refresh);
        board.add_child(actions);
    }

    _setBar(tile, pct) {
        const w = Math.max(0, Math.min(100, pct)) / 100 * BAR_W;
        tile.fill.set_width(Math.round(w));
    }

    _offline() {
        if (this._destroyed) return;
        this._label.set_text('离线');
        this._host.set_text('守护进程未运行');
        for (const k of ['cpu', 'mem', 'net', 'tmp']) {
            this._tiles[k].val.set_text('--');
            this._tiles[k].sub.set_text('sysmon --port 8787');
            this._setBar(this._tiles[k], 0);
        }
        for (const r of this._procRows) {
            r.nm.set_text('--');
            r.sb.set_text('');
            r.cpu.set_text('');
            r.mem.set_text('');
        }
    }

    _refresh() {
        if (this._destroyed) return;
        let msg;
        try {
            msg = Soup.Message.new('GET', API);
        } catch (e) {
            this._offline();
            return;
        }
        this._session.send_and_read_async(msg, GLib.PRIORITY_DEFAULT, null, (session, res) => {
            if (this._destroyed) return;
            try {
                const bytes = session.send_and_read_finish(res);
                this._apply(JSON.parse(new TextDecoder().decode(bytes.get_data())));
            } catch (e) {
                this._offline();
            }
        });
    }

    _apply(s) {
        const total = s.mem.total || 1;
        const used = total - s.mem.avail;
        const memPct = used * 100 / total;

        this._label.set_text(Math.round(s.cpu.pct) + '% · ' + Math.round(memPct) + '%');

        const up = s.uptime || 0;
        const h = Math.floor((up % 86400) / 3600);
        const m = Math.floor((up % 3600) / 60);
        this._host.set_text(s.host.name + ' · 运行 ' +
            (up >= 86400 ? Math.floor(up / 86400) + ' 天 ' : '') + h + ' 小时 ' + m + ' 分');

        const cpu = s.cpu.pct;
        const t = this._tiles;
        t.cpu.val.set_text(cpu.toFixed(1) + '%');
        t.cpu.sub.set_text('load ' + s.cpu.load1.toFixed(2) + ' · ' + s.cpu.cores.length + ' 线程');
        this._setBar(t.cpu, cpu);

        t.mem.val.set_text(memPct.toFixed(0) + '%');
        t.mem.sub.set_text(fmtGB(used) + ' / ' + fmtGB(total) + ' GB');
        this._setBar(t.mem, memPct);

        let rx = 0, tx = 0, nic = '-';
        for (const n of s.net) {
            rx += n.rx_bps;
            tx += n.tx_bps;
            nic = n.name;
        }
        t.net.val.set_text(fmtRate(rx));
        t.net.sub.set_text('↑ ' + fmtRate(tx) + ' · ' + nic);
        this._setBar(t.net, rx / 1048576 * 100);

        let tmax = 0, tname = '';
        for (const x of s.temp) {
            if (/nvme|ssd/i.test(x.chip)) continue;
            if (x.c > tmax) {
                tmax = x.c;
                tname = x.label;
            }
        }
        t.tmp.val.set_text(tmax ? tmax.toFixed(0) + '°C' : '--');
        t.tmp.sub.set_text((tname || '传感器') +
            (s.batt.present ? ' · 电池 ' + s.batt.pct.toFixed(0) + '%' : ''));
        this._setBar(t.tmp, tmax);

        const procs = [...(s.procs || [])].sort((a, b) => b.cpu - a.cpu).slice(0, 3);
        for (let i = 0; i < this._procRows.length; i++) {
            const r = this._procRows[i];
            const p = procs[i];
            if (!p) {
                r.nm.set_text('—');
                r.sb.set_text('');
                r.cpu.set_text('');
                r.mem.set_text('');
                continue;
            }
            const full = p.cmd || p.name;
            const app = full.split(' ')[0].split('/').pop();
            r.ico.set_text(app.slice(0, 2).toUpperCase());
            r.nm.set_text(app);
            r.sb.set_text('pid ' + p.pid + ' · ' + (p.user || ''));
            r.cpu.set_text(p.cpu.toFixed(1) + '%');
            r.mem.set_text((p.rss / GB).toFixed(1) + 'G');
        }
    }

    destroy() {
        this._destroyed = true;
        if (this._timer) {
            GLib.source_remove(this._timer);
            this._timer = 0;
        }
        super.destroy();
    }
});

export default class SysmonExtension extends Extension {
    enable() {
        this._indicator = new SysmonIndicator();
        Main.panel.addToStatusArea('sysmon', this._indicator, 0, 'right');
    }

    disable() {
        this._indicator?.destroy();
        this._indicator = null;
    }
}
