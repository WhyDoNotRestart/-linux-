#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <locale.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>
#include <wchar.h>
#include <ncurses.h>
#include "state.h"
#include "audit.h"
#include "apps.h"

#define HIST 80

static collector_t col;
static snapshot_t snap;

static int tab = 0;           /* 0 overview 1 procs 2 apps 3 devices 4 system */
static int sort_cpu = 0;
static int sel = 0;
static int paused = 0;
static int quit_flag = 0;

static double hist_rx[HIST];
static int hist_n = 0;
static char status[128];

static char ftext[64];
static int filter_len = 0;
static int filter_mode = 0;
static int idx[256];
static int nidx = 0;

static app_t g_apps[MAX_APPS];
static int g_napps = 0;
static int sel_app = 0;

static const char *SPARK[] = { "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█" };
static const char *TAB_NAME[] = { "总览", "进程", "应用", "设备", "系统" };

static void set_status(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(status, sizeof(status), fmt, ap);
    va_end(ap);
}

static void mkdir_parents(const char *path)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    char *slash = strrchr(tmp, '/');
    if (!slash) return;
    *slash = 0;
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    }
    mkdir(tmp, 0755);
}

static void init_audit(void)
{
    const char *xdg = getenv("XDG_STATE_HOME");
    const char *home = getenv("HOME");
    char path[512];
    if (xdg && *xdg) snprintf(path, sizeof(path), "%s/sysmon/kill.log", xdg);
    else if (home && *home) snprintf(path, sizeof(path), "%s/.local/state/sysmon/kill.log", home);
    else return;
    mkdir_parents(path);
    audit_init(path);
}

static void fmt_rate(double bps, char *out, size_t cap)
{
    if (bps >= 1048576) snprintf(out, cap, "%.2f MB/s", bps / 1048576);
    else if (bps >= 1024) snprintf(out, cap, "%.0f KB/s", bps / 1024);
    else snprintf(out, cap, "%.0f B/s", bps);
}

static void fmt_gb(unsigned long long b, char *out, size_t cap)
{
    snprintf(out, cap, "%.1fG", (double)b / 1073741824.0);
}

/* 按显示宽度截断，避免把 UTF-8 多字节字符切一半 */
static void cut_utf8(char *dst, size_t cap, const char *src, int maxcols)
{
    int col = 0;
    size_t i = 0;
    while (src[i] && col < maxcols && i + 1 < cap) {
        unsigned char c = (unsigned char)src[i];
        int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
        int w = 1;
        if (len > 1) {
            wchar_t wc;
            mbstate_t st;
            memset(&st, 0, sizeof(st));
            size_t r = mbrtowc(&wc, src + i, (size_t)len, &st);
            w = (r == (size_t)-1 || r == 0) ? 1 : wcwidth(wc);
            if (w < 0) w = 1;
        }
        if (col + w > maxcols) break;
        memcpy(dst + i, src + i, (size_t)len);
        i += (size_t)len;
        col += w;
    }
    dst[i] = 0;
}

/* argv[0] 的 basename（只取第一个参数） */
static const char *app_name(const char *cmd)
{
    static char buf[128];
    size_t n = 0;
    while (cmd[n] && cmd[n] != ' ' && n + 1 < sizeof(buf)) n++;
    memcpy(buf, cmd, n);
    buf[n] = 0;
    const char *p = strrchr(buf, '/');
    return p ? p + 1 : buf;
}

static const char *base_name(const char *cmd)
{
    const char *p = strrchr(cmd, '/');
    return p ? p + 1 : cmd;
}

static void draw_box(WINDOW *w, const char *title, int pair)
{
    int h = getmaxy(w), wd = getmaxx(w);
    werase(w);
    for (int x = 1; x < wd - 1; x++) {
        mvwaddstr(w, 0, x, "─");
        mvwaddstr(w, h - 1, x, "─");
    }
    for (int y = 1; y < h - 1; y++) {
        mvwaddstr(w, y, 0, "│");
        mvwaddstr(w, y, wd - 1, "│");
    }
    mvwaddstr(w, 0, 0, "┌");
    mvwaddstr(w, 0, wd - 1, "┐");
    mvwaddstr(w, h - 1, 0, "└");
    mvwaddstr(w, h - 1, wd - 1, "┘");
    wattron(w, COLOR_PAIR(pair) | A_BOLD);
    mvwprintw(w, 0, 2, " %s ", title);
    wattroff(w, COLOR_PAIR(pair) | A_BOLD);
}

static void draw_bar(WINDOW *w, int y, int x, int width, double pct, int pair)
{
    if (width < 4) return;
    int fill = (int)(pct * width / 100.0 + 0.5);
    if (fill > width) fill = width;
    if (fill < 0) fill = 0;
    wattron(w, COLOR_PAIR(pair));
    for (int i = 0; i < width; i++)
        mvwaddstr(w, y, x + i, i < fill ? "█" : "░");
    wattroff(w, COLOR_PAIR(pair));
}

static void draw_spark(WINDOW *w, int y, int x, int width, double *v, int n)
{
    if (n < 2 || width < 4) return;
    double max = 1;
    for (int i = 0; i < n; i++) if (v[i] > max) max = v[i];
    int start = n > width ? n - width : 0;
    for (int i = start; i < n; i++) {
        int lvl = (int)(v[i] / max * 7.0);
        if (lvl < 0) lvl = 0;
        if (lvl > 7) lvl = 7;
        mvwaddstr(w, y, x + (i - start), SPARK[lvl]);
    }
}

static int proc_cmp_cpu(const void *a, const void *b)
{
    const proc_t *x = a, *y = b;
    return x->cpu < y->cpu ? 1 : x->cpu > y->cpu ? -1 : 0;
}

static int proc_cmp_rss(const void *a, const void *b)
{
    const proc_t *x = a, *y = b;
    return x->rss < y->rss ? 1 : x->rss > y->rss ? -1 : 0;
}

static void rebuild_filter(void)
{
    nidx = 0;
    for (int i = 0; i < snap.nproc && nidx < 256; i++) {
        if (filter_len == 0) { idx[nidx++] = i; continue; }
        char hay[256];
        snprintf(hay, sizeof(hay), "%s %s %s", snap.proc[i].name, snap.proc[i].cmd, snap.proc[i].user);
        if (strcasestr(hay, ftext)) idx[nidx++] = i;
    }
}

static void draw_header(void)
{
    unsigned long long u = snap.uptime;
    attron(A_BOLD);
    mvprintw(0, 0, " sysmon ");
    attroff(A_BOLD);
    attron(COLOR_PAIR(5));
    printw(" %s  ·  %s  ·  up %llud %lluh ", snap.host, snap.distro, u / 86400, (u % 86400) / 3600);
    attroff(COLOR_PAIR(5));
    if (paused) {
        attron(COLOR_PAIR(3) | A_BOLD);
        printw(" PAUSED ");
        attroff(COLOR_PAIR(3) | A_BOLD);
    }
}

static void draw_tabs(void)
{
    move(1, 0);
    for (int i = 0; i < 5; i++) {
        if (i == tab) {
            attron(COLOR_PAIR(2) | A_REVERSE | A_BOLD);
            printw(" %d %s ", i + 1, TAB_NAME[i]);
            attroff(COLOR_PAIR(2) | A_REVERSE | A_BOLD);
        } else {
            attron(COLOR_PAIR(5));
            printw(" %d %s ", i + 1, TAB_NAME[i]);
            attroff(COLOR_PAIR(5));
        }
    }
    attron(COLOR_PAIR(5));
    printw("   Tab 切换");
    attroff(COLOR_PAIR(5));
}

static void tab_overview(void)
{
    WINDOW *c = newwin(5, COLS, 2, 0);
    draw_box(c, "CPU", 2);
    mvwprintw(c, 1, 2, "%.1f%%", snap.cpu_pct);
    mvwprintw(c, 1, 10, "load %.2f %.2f %.2f", snap.load1, snap.load5, snap.load15);
    mvwprintw(c, 1, 40, "%d 线程", snap.ncores);
    draw_bar(c, 2, 2, COLS - 6, snap.cpu_pct,
             snap.cpu_pct > 85 ? 4 : snap.cpu_pct > 60 ? 3 : 1);

    int per = (COLS - 4) / (snap.ncores > 0 ? snap.ncores : 1);
    if (per < 2) per = 2;
    for (int i = 0; i < snap.ncores && 2 + i * per < COLS - 2; i++) {
        int lvl = (int)(snap.core[i] / 100.0 * 7.0);
        if (lvl < 0) lvl = 0;
        if (lvl > 7) lvl = 7;
        int pair = snap.core[i] > 80 ? 4 : 1;
        wattron(c, COLOR_PAIR(pair));
        mvwaddstr(c, 3, 2 + i * per, SPARK[lvl]);
        wattroff(c, COLOR_PAIR(pair));
    }
    wrefresh(c); delwin(c);

    int half = COLS / 2;
    WINDOW *m = newwin(4, half, 7, 0);
    WINDOW *n = newwin(4, COLS - half, 7, half);

    draw_box(m, "内存", 2);
    char used[16], total[16], avail[16], sw[16], swt[16];
    fmt_gb(snap.mem_total - snap.mem_avail, used, sizeof(used));
    fmt_gb(snap.mem_total, total, sizeof(total));
    fmt_gb(snap.mem_avail, avail, sizeof(avail));
    fmt_gb(snap.swap_total - snap.swap_free, sw, sizeof(sw));
    fmt_gb(snap.swap_total, swt, sizeof(swt));
    double pct = snap.mem_total ? (double)(snap.mem_total - snap.mem_avail) * 100.0 / snap.mem_total : 0;
    mvwprintw(m, 1, 2, "%s / %s  %.0f%%", used, total, pct);
    mvwprintw(m, 1, half - 12, "可用 %s", avail);
    int barw = half - 20;
    draw_bar(m, 2, 2, barw, pct, 6);
    mvwprintw(m, 2, 2 + barw + 1, "S %s/%s", sw, swt);

    draw_box(n, "网络", 2);
    char rx[24], tx[24];
    double rxb = 0, txb = 0;
    const char *nic = snap.nnet ? snap.net[0].name : "-";
    for (int i = 0; i < snap.nnet; i++) { rxb += snap.net[i].rx_bps; txb += snap.net[i].tx_bps; }
    fmt_rate(rxb, rx, sizeof(rx));
    fmt_rate(txb, tx, sizeof(tx));
    mvwprintw(n, 1, 2, "%s  ↓ %s  ↑ %s", nic, rx, tx);
    draw_spark(n, 2, 2, COLS - half - 4, hist_rx, hist_n);
    wrefresh(m); wrefresh(n); delwin(m); delwin(n);

    WINDOW *t = newwin(3, half, 11, 0);
    WINDOW *d = newwin(3, COLS - half, 11, half);
    draw_box(t, "温度", 2);
    int x = 2;
    const char *seen[8];
    int ns = 0;
    for (int i = 0; i < snap.ntemp && x < half - 12; i++) {
        const char *nm = snap.temp[i].label;
        if (!strncmp(nm, "Sensor", 6)) nm = snap.temp[i].chip;
        int dup = 0;
        for (int k = 0; k < ns; k++) if (!strcmp(seen[k], nm)) dup = 1;
        if (dup) continue;
        if (ns < 8) seen[ns++] = nm;
        x += mvwprintw(t, 1, x, "%s %.0f°  ", nm, snap.temp[i].c);
    }
    draw_box(d, "磁盘", 2);
    double fs = snap.fs_total ? (double)(snap.fs_total - snap.fs_free) * 100.0 / snap.fs_total : 0;
    char r[24], w2[24];
    fmt_rate(snap.ndisk ? snap.disk[0].rbps : 0, r, sizeof(r));
    fmt_rate(snap.ndisk ? snap.disk[0].wbps : 0, w2, sizeof(w2));
    mvwprintw(d, 1, 2, "/ %.0f%%   读 %s   写 %s", fs, r, w2);
    wrefresh(t); wrefresh(d); delwin(t); delwin(d);

    int ph = LINES - 15;
    if (ph >= 5) {
        WINDOW *p = newwin(ph, COLS, 14, 0);
        draw_box(p, "占用最高的进程", 2);
        qsort(snap.proc, (size_t)snap.nproc, sizeof(proc_t), proc_cmp_cpu);
        int max = ph - 2;
        for (int i = 0; i < snap.nproc && i < max; i++) {
            proc_t *pr = &snap.proc[i];
            const char *app = pr->cmd[0] ? app_name(pr->cmd) : pr->name;
            char appc[64];
            cut_utf8(appc, sizeof(appc), app, 18);
            mvwprintw(p, 1 + i, 2, "%-7d %-18s %5.1f%%  %5.0fM",
                      pr->pid, appc, pr->cpu, (double)pr->rss / 1048576.0);
        }
        wrefresh(p); delwin(p);
    }
}

static void tab_procs(void)
{
    int h = LINES - 3;
    if (h < 4) return;
    WINDOW *w = newwin(h, COLS, 2, 0);

    if (sort_cpu) qsort(snap.proc, (size_t)snap.nproc, sizeof(proc_t), proc_cmp_cpu);
    else qsort(snap.proc, (size_t)snap.nproc, sizeof(proc_t), proc_cmp_rss);
    rebuild_filter();

    char title[96];
    if (filter_len)
        snprintf(title, sizeof(title), "进程 · 匹配 %d / %d · 按%s排序", nidx, snap.nproc, sort_cpu ? "CPU" : "内存");
    else
        snprintf(title, sizeof(title), "进程 · 共 %d 个 · 按%s排序", snap.nproc, sort_cpu ? "CPU" : "内存");
    draw_box(w, title, 2);

    attron(A_BOLD | COLOR_PAIR(5));
    mvwprintw(w, 1, 1,  "PID");
    mvwprintw(w, 1, 8,  "USER");
    mvwprintw(w, 1, 19, "CPU%");
    mvwprintw(w, 1, 26, "MEM");
    mvwprintw(w, 1, 33, "应用");
    mvwprintw(w, 1, 51, "命令");
    attroff(A_BOLD | COLOR_PAIR(5));

    int rows = h - 4;
    if (sel >= nidx) sel = nidx ? nidx - 1 : 0;
    if (sel < 0) sel = 0;
    int off = sel >= rows ? sel - rows + 1 : 0;
    int cmdw = COLS - 56;
    if (cmdw < 8) cmdw = 8;

    for (int i = 0; i < rows && off + i < nidx; i++) {
        proc_t *p = &snap.proc[idx[off + i]];
        const char *app = p->cmd[0] ? app_name(p->cmd) : p->name;
        char appc[64], cmdc[256], userc[32];
        cut_utf8(appc, sizeof(appc), app, 15);
        cut_utf8(cmdc, sizeof(cmdc), p->cmd, cmdw);
        cut_utf8(userc, sizeof(userc), p->user, 9);
        if (off + i == sel) wattron(w, A_REVERSE);
        mvwprintw(w, 2 + i, 1,  "%-6d", p->pid);
        mvwprintw(w, 2 + i, 8,  "%-9s", userc);
        mvwprintw(w, 2 + i, 19, "%5.1f%%", p->cpu);
        mvwprintw(w, 2 + i, 26, "%5.0fM", (double)p->rss / 1048576.0);
        mvwprintw(w, 2 + i, 33, "%-16s", appc);
        mvwprintw(w, 2 + i, 51, "%s", cmdc);
        if (off + i == sel) wattroff(w, A_REVERSE);
    }

    if (nidx > 0) {
        proc_t *p = &snap.proc[idx[sel]];
        attron(COLOR_PAIR(5));
        mvwprintw(w, h - 2, 1, "ppid %-6d %-3s  pid %-6d  %.*s",
                  p->ppid, p->state, p->pid, COLS - 34, p->cmd);
        attroff(COLOR_PAIR(5));
    } else {
        mvwprintw(w, 2, 1, filter_len ? "没有匹配的进程" : "没有可见进程");
    }

    wrefresh(w); delwin(w);
}

static void tab_apps(void)
{
    int h = LINES - 3;
    if (h < 4) return;
    WINDOW *w = newwin(h, COLS, 2, 0);

    g_napps = apps_group(snap.proc, snap.nproc, g_apps, MAX_APPS);

    char title[96];
    snprintf(title, sizeof(title), "应用 · 共 %d 个 · 按内存排序", g_napps);
    draw_box(w, title, 2);

    attron(A_BOLD | COLOR_PAIR(5));
    mvwprintw(w, 1, 1,  "应用");
    mvwprintw(w, 1, 30, "进程");
    mvwprintw(w, 1, 39, "CPU");
    mvwprintw(w, 1, 48, "内存");
    attroff(A_BOLD | COLOR_PAIR(5));

    int rows = h - 4;
    if (sel_app >= g_napps) sel_app = g_napps ? g_napps - 1 : 0;
    if (sel_app < 0) sel_app = 0;
    int off = sel_app >= rows ? sel_app - rows + 1 : 0;

    for (int i = 0; i < rows && off + i < g_napps; i++) {
        app_t *a = &g_apps[off + i];
        char nm[48];
        cut_utf8(nm, sizeof(nm), a->display, 26);
        if (off + i == sel_app) wattron(w, A_REVERSE);
        mvwprintw(w, 2 + i, 1,  "%-26s %5d  %5.1f%%  %6.0fM",
                  nm, a->npid, a->cpu, (double)a->rss / 1048576.0);
        if (off + i == sel_app) wattroff(w, A_REVERSE);
    }

    if (g_napps > 0) {
        app_t *a = &g_apps[sel_app];
        attron(COLOR_PAIR(5));
        mvwprintw(w, h - 2, 1, "%s · %d 个进程 · pid %d ..",
                  a->app, a->npid, a->npid ? a->pids[0] : 0);
        attroff(COLOR_PAIR(5));
    } else {
        mvwprintw(w, 2, 1, "没有可见应用");
    }

    wrefresh(w); delwin(w);
}

static void confirm_kill_app(void)
{
    if (g_napps == 0) return;
    app_t *a = &g_apps[sel_app];

    int h = 6, wd = 68;
    WINDOW *dlg = newwin(h, wd, (LINES - h) / 2, (COLS - wd) / 2);
    box(dlg, 0, 0);
    wattron(dlg, COLOR_PAIR(3) | A_BOLD);
    mvwprintw(dlg, 0, 2, " 结束应用 ");
    wattroff(dlg, COLOR_PAIR(3) | A_BOLD);
    mvwprintw(dlg, 1, 2, "%.56s", a->display);
    mvwprintw(dlg, 2, 2, "将结束它的 %d 个进程，发送 SIGTERM？ y/N", a->npid);
    wrefresh(dlg);
    int ch = wgetch(dlg);
    delwin(dlg);
    if (ch != 'y' && ch != 'Y') return;

    int ok = 0, fail = 0;
    for (int i = 0; i < a->npid; i++) {
        int pid = a->pids[i];
        if (pid <= 1 || pid == getpid()) { fail++; continue; }
        char pp[64];
        struct stat sb;
        snprintf(pp, sizeof(pp), "/proc/%d", pid);
        if (stat(pp, &sb) < 0 || sb.st_uid != getuid()) { fail++; continue; }
        if (kill(pid, SIGTERM) == 0) ok++;
        else fail++;
    }
    audit_log("kill-app app=%s killed=%d failed=%d peer=tui", a->app, ok, fail);
    set_status("已结束 %s（%d 个进程）", a->display, ok);
}

static void tab_devices(void)
{
    int h = LINES - 3;
    WINDOW *w = newwin(h, COLS, 2, 0);
    draw_box(w, "设备", 2);

    int y = 1;
    wattron(w, COLOR_PAIR(2) | A_BOLD);
    mvwprintw(w, y++, 1, "USB (%d)", snap.nusb);
    wattroff(w, COLOR_PAIR(2) | A_BOLD);
    for (int i = 0; i < snap.nusb && y < h - 1; i++, y++)
        mvwprintw(w, y, 2, "%-32.32s %s:%s  bus %s",
                  snap.usb[i].name, snap.usb[i].vid, snap.usb[i].pid, snap.usb[i].bus);

    if (y < h - 1) y++;
    wattron(w, COLOR_PAIR(2) | A_BOLD);
    mvwprintw(w, y++, 1, "显示输出 (%d)", snap.ndrm);
    wattroff(w, COLOR_PAIR(2) | A_BOLD);
    for (int i = 0; i < snap.ndrm && y < h - 1; i++, y++)
        mvwprintw(w, y, 2, "%-28.28s %s", snap.drm[i].name, snap.drm[i].status);

    if (y < h - 1) y++;
    wattron(w, COLOR_PAIR(2) | A_BOLD);
    mvwprintw(w, y++, 1, "其它");
    wattroff(w, COLOR_PAIR(2) | A_BOLD);
    if (y < h - 1) {
        char sw[16], swt[16];
        fmt_gb(snap.swap_total - snap.swap_free, sw, sizeof(sw));
        fmt_gb(snap.swap_total, swt, sizeof(swt));
        mvwprintw(w, y++, 2, "PCI 设备          %d 个", snap.npci);
        if (y < h - 1) mvwprintw(w, y++, 2, "网卡              %s", snap.nnet ? snap.net[0].name : "-");
        if (y < h - 1) mvwprintw(w, y++, 2, "电池              %s %.0f%% %.1fW",
                                 snap.has_batt ? snap.batt_status : "无电池", snap.batt_pct, snap.batt_w);
        if (y < h - 1) mvwprintw(w, y++, 2, "传感器            %d 个", snap.ntemp);
        if (y < h - 1) mvwprintw(w, y++, 2, "交换              %s / %s", sw, swt);
    }

    wrefresh(w); delwin(w);
}

static void tab_system(void)
{
    int h = LINES - 3;
    WINDOW *w = newwin(h, COLS, 2, 0);
    draw_box(w, "系统信息", 2);

    char mem[48], sw[48], fsb[48];
    snprintf(mem, sizeof(mem), "%.1f GB（可用 %.1f GB）",
             (double)snap.mem_total / 1073741824.0, (double)snap.mem_avail / 1073741824.0);
    snprintf(sw, sizeof(sw), "%.1f GB（已用 %.1f GB）",
             (double)snap.swap_total / 1073741824.0, (double)(snap.swap_total - snap.swap_free) / 1073741824.0);
    double fs = snap.fs_total ? (double)(snap.fs_total - snap.fs_free) * 100.0 / snap.fs_total : 0;
    snprintf(fsb, sizeof(fsb), "%.0f GB（已用 %.0f%%）", (double)snap.fs_total / 1073741824.0, fs);

    int y = 1;
    mvwprintw(w, y++, 2, "主机        %s", snap.host);
    if (y < h - 1) mvwprintw(w, y++, 2, "发行版      %s", snap.distro);
    if (y < h - 1) mvwprintw(w, y++, 2, "内核        %s", snap.kernel);
    if (y < h - 1) mvwprintw(w, y++, 2, "CPU         %s", snap.cpu_model);
    if (y < h - 1) mvwprintw(w, y++, 2, "线程        %d", snap.ncores);
    if (y < h - 1) mvwprintw(w, y++, 2, "运行时长    %llu 天 %llu 小时",
                              snap.uptime / 86400, (snap.uptime % 86400) / 3600);
    if (y < h - 1) mvwprintw(w, y++, 2, "内存        %s", mem);
    if (y < h - 1) mvwprintw(w, y++, 2, "交换        %s", sw);
    if (y < h - 1) mvwprintw(w, y++, 2, "根分区      %s", fsb);
    if (y < h - 1) mvwprintw(w, y++, 2, "PSI 内存     some %.2f  full %.2f", snap.psi_some10, snap.psi_full10);

    wrefresh(w); delwin(w);
}

static void draw_footer(void)
{
    if (filter_mode) {
        attron(COLOR_PAIR(2));
        mvprintw(LINES - 1, 0, " 过滤: %s", ftext);
        addstr("▏");
        attroff(COLOR_PAIR(2));
        attron(COLOR_PAIR(5));
        addstr("   Enter 应用  Esc 清空  ← 删除");
        attroff(COLOR_PAIR(5));
        return;
    }

    attron(COLOR_PAIR(5));
    if (tab == 1 || tab == 2) {
        mvprintw(LINES - 1, 0, " 1-5 切换  Tab 下一屏  ↑↓ 选择  %s  x SIGTERM  X SIGKILL  q 退出 ",
                 tab == 1 ? "s 排序  / 过滤" : "整组结束");
        if (filter_len) {
            attron(COLOR_PAIR(3));
            printw("  [%s]", ftext);
            attroff(COLOR_PAIR(3));
        }
    } else {
        mvprintw(LINES - 1, 0, " 1-5 切换  Tab 下一屏  q 退出  p 暂停 ");
    }
    attroff(COLOR_PAIR(5));

    if (status[0]) {
        attron(COLOR_PAIR(3));
        mvprintw(LINES - 1, COLS - (int)strlen(status) - 2, "%s", status);
        attroff(COLOR_PAIR(3));
    }
}

static void redraw(void)
{
    erase();
    draw_header();
    draw_tabs();
    draw_footer();
    refresh();

    switch (tab) {
    case 0: tab_overview(); break;
    case 1: tab_procs(); break;
    case 2: tab_apps(); break;
    case 3: tab_devices(); break;
    case 4: tab_system(); break;
    }
    doupdate();
}

static void confirm_kill(int sig)
{
    if (tab != 1 || nidx == 0) return;
    proc_t *p = &snap.proc[idx[sel]];

    if (p->pid <= 1) { set_status("拒绝：PID 1"); return; }
    if (p->pid == getpid()) { set_status("拒绝：自身"); return; }
    if (p->uid != getuid()) { set_status("拒绝：其他用户"); return; }

    int h = 6, w = 64;
    WINDOW *dlg = newwin(h, w, (LINES - h) / 2, (COLS - w) / 2);
    box(dlg, 0, 0);
    wattron(dlg, COLOR_PAIR(3) | A_BOLD);
    mvwprintw(dlg, 0, 2, " 结束进程 ");
    wattroff(dlg, COLOR_PAIR(3) | A_BOLD);
    mvwprintw(dlg, 1, 2, "%.24s (pid %d)  %.0f MB", base_name(p->cmd), p->pid, (double)p->rss / 1048576.0);
    mvwprintw(dlg, 2, 2, "%.*s", w - 4, p->cmd);
    mvwprintw(dlg, 3, 2, "发送 SIG%s？ y/N", sig == SIGKILL ? "KILL" : "TERM");
    wrefresh(dlg);

    int ch = wgetch(dlg);
    delwin(dlg);

    if (ch == 'y' || ch == 'Y') {
        if (kill(p->pid, sig) == 0) {
            audit_log("kill ok pid=%d uid=%u name=%s cmd=%s signal=%s peer=tui",
                      p->pid, p->uid, p->name, p->cmd, sig == SIGKILL ? "KILL" : "TERM");
            set_status("已发送 SIG%s → %s", sig == SIGKILL ? "KILL" : "TERM", base_name(p->cmd));
        } else {
            audit_log("kill failed pid=%d name=%s signal=%s err=%s",
                      p->pid, p->name, sig == SIGKILL ? "KILL" : "TERM", strerror(errno));
            set_status("失败: %s", strerror(errno));
        }
    }
}

int main(int argc, char **argv)
{
    int frames = 0;
    int start_tab = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--tab") && i + 1 < argc) start_tab = atoi(argv[++i]) - 1;
        else if (!strcmp(argv[i], "--filter") && i + 1 < argc) {
            snprintf(ftext, sizeof(ftext), "%s", argv[++i]);
            filter_len = (int)strlen(ftext);
        }
    }
    if (start_tab < 0 || start_tab > 4) start_tab = 0;

    setlocale(LC_ALL, "");

    collect_init(&col, &snap);
    col.proc_interval = 2000;
    tab = start_tab;
    init_audit();

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    timeout(200);
    set_escdelay(25);
    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(1, COLOR_GREEN, -1);
        init_pair(2, COLOR_CYAN, -1);
        init_pair(3, COLOR_YELLOW, -1);
        init_pair(4, COLOR_RED, -1);
        init_pair(5, COLOR_WHITE, -1);
        init_pair(6, COLOR_MAGENTA, -1);
    }

    time_t last = 0;

    for (;;) {
        time_t now = time(NULL);
        if (!paused && now != last) {
            last = now;
            collect_tick(&col, &snap);

            double rx = 0;
            for (int i = 0; i < snap.nnet; i++) rx += snap.net[i].rx_bps;
            if (hist_n < HIST) hist_rx[hist_n++] = rx / 1024.0;
            else {
                memmove(hist_rx, hist_rx + 1, sizeof(double) * (HIST - 1));
                hist_rx[HIST - 1] = rx / 1024.0;
            }
            if (status[0] && now % 5 == 0) status[0] = 0;
            redraw();
            if (frames && --frames == 0) break;
        }

        int ch = getch();

        if (filter_mode) {
            if (ch == 27) { filter_mode = 0; filter_len = 0; ftext[0] = 0; sel = 0; redraw(); }
            else if (ch == '\n' || ch == KEY_ENTER) { filter_mode = 0; redraw(); }
            else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
                if (filter_len > 0) ftext[--filter_len] = 0;
                sel = 0; redraw();
            } else if (ch >= 32 && ch < 127 && filter_len < (int)sizeof(ftext) - 1) {
                ftext[filter_len++] = (char)ch;
                ftext[filter_len] = 0;
                sel = 0; redraw();
            }
            continue;
        }

        if (ch == 'q' || ch == 'Q') break;
        else if (ch >= '1' && ch <= '5') { tab = ch - '1'; sel = 0; sel_app = 0; redraw(); }
        else if (ch == '\t') { tab = (tab + 1) % 5; sel = 0; sel_app = 0; redraw(); }
        else if (ch == KEY_BTAB) { tab = (tab + 4) % 5; sel = 0; sel_app = 0; redraw(); }
        else if (ch == 's' || ch == 'S') { sort_cpu = !sort_cpu; sel = 0; redraw(); }
        else if (ch == '/' && tab == 1) { filter_mode = 1; redraw(); }
        else if (ch == 'p' || ch == 'P') { paused = !paused; redraw(); }
        else if (ch == KEY_UP && tab == 1 && sel > 0) { sel--; redraw(); }
        else if (ch == KEY_DOWN && tab == 1 && sel + 1 < nidx) { sel++; redraw(); }
        else if (ch == KEY_UP && tab == 2 && sel_app > 0) { sel_app--; redraw(); }
        else if (ch == KEY_DOWN && tab == 2 && sel_app + 1 < g_napps) { sel_app++; redraw(); }
        else if (ch == 'x') {
            if (tab == 2) confirm_kill_app();
            else confirm_kill(SIGTERM);
            redraw();
        }
        else if (ch == 'X') {
            if (tab == 2) confirm_kill_app();
            else confirm_kill(SIGKILL);
            redraw();
        }
        else if (ch == KEY_RESIZE) redraw();
    }

    endwin();
    return 0;
}
