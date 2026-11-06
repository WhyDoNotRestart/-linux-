#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <time.h>
#include <sys/utsname.h>
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <pwd.h>
#include "state.h"

static int read_file(const char *path, char *buf, size_t cap)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, cap - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = 0;
    return (int)n;
}

static unsigned long long meminfo_kb(const char *buf, const char *key)
{
    const char *p = strstr(buf, key);
    if (!p) return 0;
    return strtoull(p + strlen(key), NULL, 10);
}

static double now_ts(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int want_disk(const char *name)
{
    if (!strncmp(name, "nvme", 4)) return strchr(name, 'p') == NULL;
    if (!strncmp(name, "sd", 2)) return 1;
    if (!strncmp(name, "vd", 2)) return 1;
    if (!strncmp(name, "mmcblk", 6)) return strchr(name, 'p') == NULL;
    return 0;
}

static void read_hostinfo(snapshot_t *s)
{
    struct utsname u;
    if (uname(&u) == 0) {
        snprintf(s->kernel, sizeof(s->kernel), "%s", u.release);
        snprintf(s->host, sizeof(s->host), "%s", u.nodename);
    }

    char buf[4096];
    if (read_file("/etc/os-release", buf, sizeof(buf)) > 0) {
        const char *p = strstr(buf, "PRETTY_NAME=\"");
        if (p) {
            p += 13;
            const char *e = strchr(p, '"');
            if (e) snprintf(s->distro, sizeof(s->distro), "%.*s", (int)(e - p), p);
        }
    }

    if (read_file("/proc/cpuinfo", buf, sizeof(buf)) > 0) {
        const char *p = strstr(buf, "model name");
        if (p && (p = strchr(p, ':')) != NULL) {
            while (*++p == ' ') {}
            const char *e = strchr(p, '\n');
            if (e) snprintf(s->cpu_model, sizeof(s->cpu_model), "%.*s", (int)(e - p), p);
        }
    }
}

static void read_mem(snapshot_t *s)
{
    char buf[4096];
    if (read_file("/proc/meminfo", buf, sizeof(buf)) < 0) return;
    s->mem_total      = meminfo_kb(buf, "MemTotal:") * 1024ULL;
    s->mem_avail      = meminfo_kb(buf, "MemAvailable:") * 1024ULL;
    s->mem_free       = meminfo_kb(buf, "MemFree:") * 1024ULL;
    s->mem_cached     = meminfo_kb(buf, "Cached:") * 1024ULL;
    s->mem_buffers    = meminfo_kb(buf, "Buffers:") * 1024ULL;
    s->mem_sreclaim   = meminfo_kb(buf, "SReclaimable:") * 1024ULL;
    s->mem_shmem      = meminfo_kb(buf, "Shmem:") * 1024ULL;
    s->mem_dirty      = meminfo_kb(buf, "Dirty:") * 1024ULL;
    s->mem_writeback  = meminfo_kb(buf, "Writeback:") * 1024ULL;
    s->mem_anon       = meminfo_kb(buf, "AnonPages:") * 1024ULL;
    s->mem_slab       = meminfo_kb(buf, "Slab:") * 1024ULL;
    s->mem_pagetables = meminfo_kb(buf, "PageTables:") * 1024ULL;
    s->swap_total     = meminfo_kb(buf, "SwapTotal:") * 1024ULL;
    s->swap_free      = meminfo_kb(buf, "SwapFree:") * 1024ULL;
}

static void read_cpu(collector_t *c, snapshot_t *s)
{
    char buf[16384];
    if (read_file("/proc/stat", buf, sizeof(buf)) < 0) return;

    unsigned long long total[MAX_CORES + 1], idle[MAX_CORES + 1];
    int rows = 0;
    memset(total, 0, sizeof(total));
    memset(idle, 0, sizeof(idle));

    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (strncmp(line, "cpu", 3)) break;
        if (rows > MAX_CORES) break;
        unsigned long long v[10] = {0};
        int got = sscanf(line + 4, "%llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                         &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9]);
        if (got < 4) continue;
        unsigned long long t = 0;
        for (int i = 0; i < 10; i++) t += v[i];
        total[rows] = t;
        idle[rows] = v[3] + v[4];
        rows++;
    }
    if (rows == 0) return;

    if (!c->inited) {
        for (int i = 0; i < rows; i++) {
            c->prev_total[i] = total[i];
            c->prev_idle[i] = idle[i];
        }
        return;
    }

    for (int i = 0; i < rows; i++) {
        unsigned long long dt_t = total[i] - c->prev_total[i];
        unsigned long long dt_i = idle[i] - c->prev_idle[i];
        double pct = dt_t ? (double)(dt_t - dt_i) * 100.0 / (double)dt_t : 0.0;
        if (i == 0) s->cpu_pct = pct;
        else if (i - 1 < MAX_CORES) s->core[i - 1] = pct;
        c->prev_total[i] = total[i];
        c->prev_idle[i] = idle[i];
    }
    s->ncores = rows - 1;
    if (s->ncores > MAX_CORES) s->ncores = MAX_CORES;

    char b2[256];
    if (read_file("/proc/loadavg", b2, sizeof(b2)) > 0)
        sscanf(b2, "%lf %lf %lf", &s->load1, &s->load5, &s->load15);
}

static void read_net(collector_t *c, snapshot_t *s, double dt)
{
    char buf[8192];
    if (read_file("/proc/net/dev", buf, sizeof(buf)) < 0) return;

    char names[MAX_NET][16];
    unsigned long long rx[MAX_NET], tx[MAX_NET];
    int n = 0;

    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *colon = strchr(line, ':');
        if (!colon) continue;
        *colon = 0;
        char *name = line;
        while (*name == ' ') name++;
        if (!strcmp(name, "lo")) continue;
        if (n >= MAX_NET) break;
        unsigned long long r = 0, t = 0;
        if (sscanf(colon + 1, "%llu %*u %*u %*u %*u %*u %*u %*u %llu", &r, &t) != 2) continue;
        snprintf(names[n], sizeof(names[n]), "%s", name);
        rx[n] = r;
        tx[n] = t;
        n++;
    }

    for (int i = 0; i < n; i++) {
        snprintf(s->net[i].name, sizeof(s->net[i].name), "%s", names[i]);
        s->net[i].rx = rx[i];
        s->net[i].tx = tx[i];
        if (dt > 0) {
            s->net[i].rx_bps = (double)(rx[i] - c->prev_rx[i]) / dt;
            s->net[i].tx_bps = (double)(tx[i] - c->prev_tx[i]) / dt;
            if (rx[i] < c->prev_rx[i]) s->net[i].rx_bps = 0;
            if (tx[i] < c->prev_tx[i]) s->net[i].tx_bps = 0;
        }
        c->prev_rx[i] = rx[i];
        c->prev_tx[i] = tx[i];
        snprintf(c->net_names[i], sizeof(c->net_names[i]), "%s", names[i]);
    }
    s->nnet = n;
    c->nnet = n;
}

static void read_disk(collector_t *c, snapshot_t *s, double dt)
{
    char buf[32768];
    if (read_file("/proc/diskstats", buf, sizeof(buf)) < 0) return;

    char names[MAX_DISK][24];
    unsigned long long rsec[MAX_DISK], wsec[MAX_DISK];
    int n = 0;

    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char name[32] = {0};
        unsigned long long rs = 0, ws = 0;
        if (sscanf(line, "%*u %*u %31s %*u %*u %llu %*u %*u %*u %llu", name, &rs, &ws) != 3)
            continue;
        if (!want_disk(name)) continue;
        if (n >= MAX_DISK) break;
        snprintf(names[n], sizeof(names[n]), "%s", name);
        rsec[n] = rs;
        wsec[n] = ws;
        n++;
    }

    for (int i = 0; i < n; i++) {
        snprintf(s->disk[i].name, sizeof(s->disk[i].name), "%s", names[i]);
        s->disk[i].rd = rsec[i] * 512ULL;
        s->disk[i].wr = wsec[i] * 512ULL;
        if (dt > 0 && rsec[i] >= c->prev_rd[i] && wsec[i] >= c->prev_wr[i]) {
            s->disk[i].rbps = (double)(rsec[i] - c->prev_rd[i]) * 512.0 / dt;
            s->disk[i].wbps = (double)(wsec[i] - c->prev_wr[i]) * 512.0 / dt;
        }
        c->prev_rd[i] = rsec[i];
        c->prev_wr[i] = wsec[i];
        snprintf(c->disk_names[i], sizeof(c->disk_names[i]), "%s", names[i]);
    }
    s->ndisk = n;
    c->ndisk = n;
}

static void enum_temps(collector_t *c)
{
    c->ntemp_paths = 0;
    DIR *d = opendir("/sys/class/hwmon");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && c->ntemp_paths < MAX_TEMP) {
        if (strncmp(e->d_name, "hwmon", 5)) continue;
        char path[320], chip[64] = {0};
        snprintf(path, sizeof(path), "/sys/class/hwmon/%s/name", e->d_name);
        if (read_file(path, chip, sizeof(chip)) < 0) continue;
        chip[strcspn(chip, "\n")] = 0;

        for (int i = 1; i <= 16 && c->ntemp_paths < MAX_TEMP; i++) {
            char probe[320], buf[64];
            snprintf(probe, sizeof(probe), "/sys/class/hwmon/%s/temp%d_input", e->d_name, i);
            if (read_file(probe, buf, sizeof(buf)) < 0) continue;

            int k = c->ntemp_paths;
            snprintf(c->temp_path[k], sizeof(c->temp_path[k]), "%s", probe);
            snprintf(c->temp_chip[k], sizeof(c->temp_chip[k]), "%s", chip);

            char label[64] = {0};
            snprintf(probe, sizeof(probe), "/sys/class/hwmon/%s/temp%d_label", e->d_name, i);
            read_file(probe, label, sizeof(label));
            label[strcspn(label, "\n")] = 0;
            if (label[0]) snprintf(c->temp_label[k], sizeof(c->temp_label[k]), "%s", label);
            else snprintf(c->temp_label[k], sizeof(c->temp_label[k]), "%s #%d", chip, i);
            c->ntemp_paths++;
        }
    }
    closedir(d);
}

static void read_temps(collector_t *c, snapshot_t *s)
{
    double now = now_ts();
    if (c->ntemp_paths == 0 || now - c->last_temp_enum > 30.0) {
        enum_temps(c);
        c->last_temp_enum = now;
    }

    int n = 0;
    for (int i = 0; i < c->ntemp_paths; i++) {
        char buf[64];
        if (read_file(c->temp_path[i], buf, sizeof(buf)) < 0) continue;
        double v = atof(buf) / 1000.0;
        if (v <= 0.0 || v >= 150.0) continue;
        snprintf(s->temp[n].chip, sizeof(s->temp[n].chip), "%s", c->temp_chip[i]);
        snprintf(s->temp[n].label, sizeof(s->temp[n].label), "%s", c->temp_label[i]);
        s->temp[n].c = v;
        n++;
    }
    s->ntemp = n;
}

static void read_battery(snapshot_t *s)
{
    char buf[128];
    s->has_batt = 0;
    if (read_file("/sys/class/power_supply/BAT0/capacity", buf, sizeof(buf)) < 0) return;
    s->has_batt = 1;
    s->batt_pct = atof(buf);

    if (read_file("/sys/class/power_supply/BAT0/status", buf, sizeof(buf)) > 0) {
        buf[strcspn(buf, "\n")] = 0;
        snprintf(s->batt_status, sizeof(s->batt_status), "%s", buf);
    }
    if (read_file("/sys/class/power_supply/BAT0/power_now", buf, sizeof(buf)) > 0) {
        s->batt_w = atof(buf) / 1e6;
    } else {
        double v = 0, i = 0;
        if (read_file("/sys/class/power_supply/BAT0/voltage_now", buf, sizeof(buf)) > 0) v = atof(buf) / 1e6;
        if (read_file("/sys/class/power_supply/BAT0/current_now", buf, sizeof(buf)) > 0) i = atof(buf) / 1e6;
        s->batt_w = v * i;
    }
}

static void read_psi(snapshot_t *s)
{
    char buf[512];
    if (read_file("/proc/pressure/memory", buf, sizeof(buf)) < 0) return;
    char *p = strstr(buf, "some ");
    if (p) sscanf(p, "some avg10=%lf", &s->psi_some10);
    p = strstr(buf, "full ");
    if (p) sscanf(p, "full avg10=%lf", &s->psi_full10);
}

static void read_fs(snapshot_t *s)
{
    struct statvfs v;
    if (statvfs("/", &v) == 0) {
        s->fs_total = (unsigned long long)v.f_blocks * v.f_frsize;
        s->fs_free  = (unsigned long long)v.f_bavail * v.f_frsize;
    }
}

static int pid_cmp(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

static const char *user_name(unsigned uid)
{
    static struct { unsigned uid; char name[16]; } cache[16];
    static int n = 0;
    for (int i = 0; i < n; i++) if (cache[i].uid == uid) return cache[i].name;
    const char *nm = "?";
    struct passwd *pw = getpwuid(uid);
    if (pw && pw->pw_name) nm = pw->pw_name;
    if (n < 16) {
        cache[n].uid = uid;
        snprintf(cache[n].name, sizeof(cache[n].name), "%s", nm);
        nm = cache[n].name;
        n++;
    }
    return nm;
}

static void read_procs(collector_t *c, snapshot_t *s, double dt)
{
    DIR *d = opendir("/proc");
    if (!d) { s->nproc = 0; return; }

    struct dirent *e;
    int n = 0;
    int np = 0;
    proc_t tmp[1024];
    int pids[1024];
    unsigned long long ticks[1024];

    while ((e = readdir(d)) && n < 1024) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;

        char path[96], buf[1024];
        int pid = atoi(e->d_name);

        /* 一次 stat 读齐：状态、ppid、utime/stime、rss（第 24 字段） */
        snprintf(path, sizeof(path), "/proc/%s/stat", e->d_name);
        if (read_file(path, buf, sizeof(buf)) < 0) continue;
        char *rp = strrchr(buf, ')');
        if (!rp) continue;
        char st = 0;
        int ppid = 0;
        unsigned long long ut = 0, stt = 0;
        long long rss_pages = 0;
        if (sscanf(rp + 1,
                   " %c %d %*s %*s %*s %*s %*s %*s %*s %*s %*s %llu %llu"
                   " %*s %*s %*s %*s %*s %*s %*s %*s %lld",
                   &st, &ppid, &ut, &stt, &rss_pages) != 5) continue;
        unsigned long long rss = rss_pages > 0 ? (unsigned long long)rss_pages * 4096ULL : 0;
        unsigned long long tk = ut + stt;

        /* 内核线程：没有地址空间、没有 cmdline，监控里没有意义 */
        if (ppid == 2 && rss == 0) continue;

        double cpu = 0;
        for (int i = 0; i < c->nprev; i++) {
            if (c->prev_pids[i] == pid) {
                if (dt > 0 && tk >= c->prev_ticks[i])
                    cpu = (double)(tk - c->prev_ticks[i]) / 100.0 / dt * 100.0;
                break;
            }
        }
        if (np < 1024) { pids[np] = pid; ticks[np] = tk; np++; }

        /* 复用上一轮的名字/命令行/cgroup，只有新 PID 才读这三个文件 */
        proc_t *old = NULL;
        for (int i = 0; i < c->nproc_cache; i++)
            if (c->proc_cache[i].pid == pid) { old = &c->proc_cache[i]; break; }

        char comm[64] = {0}, cmd[256] = {0}, scope[96] = {0};

        if (old && old->name[0]) {
            snprintf(comm, sizeof(comm), "%s", old->name);
            snprintf(cmd, sizeof(cmd), "%s", old->cmd);
            snprintf(scope, sizeof(scope), "%s", old->scope);
        } else {
            snprintf(path, sizeof(path), "/proc/%s/comm", e->d_name);
            if (read_file(path, comm, sizeof(comm)) < 0) snprintf(comm, sizeof(comm), "?");
            comm[strcspn(comm, "\n")] = 0;

            snprintf(path, sizeof(path), "/proc/%s/cmdline", e->d_name);
            int clen = read_file(path, cmd, sizeof(cmd));
            if (clen > 0) {
                for (int k = 0; k < clen; k++) {
                    unsigned char ch = (unsigned char)cmd[k];
                    if (ch == 0 || ch < 0x20 || ch == 0x7f) cmd[k] = ' ';
                }
                cmd[clen] = 0;
                while (clen > 0 && cmd[clen - 1] == ' ') cmd[--clen] = 0;
                int w = 0;
                for (int k = 0; k < clen; k++) {
                    if (cmd[k] == ' ' && w > 0 && cmd[w - 1] == ' ') continue;
                    cmd[w++] = cmd[k];
                }
                cmd[w] = 0;
            }

            snprintf(path, sizeof(path), "/proc/%s/cgroup", e->d_name);
            if (read_file(path, scope, sizeof(scope)) > 0) {
                char *nl = strchr(scope, '\n');
                if (nl) *nl = 0;
                char *last = strrchr(scope, '/');
                if (last) memmove(scope, last + 1, strlen(last + 1) + 1);
            }
        }

        unsigned uid = 0;
        if (old) {
            uid = old->uid;
        } else {
            snprintf(path, sizeof(path), "/proc/%s", e->d_name);
            struct stat sb;
            if (stat(path, &sb) == 0) uid = sb.st_uid;
        }

        tmp[n].pid = pid;
        tmp[n].ppid = ppid;
        tmp[n].uid = uid;
        snprintf(tmp[n].user, sizeof(tmp[n].user), "%s", user_name(uid));
        snprintf(tmp[n].name, sizeof(tmp[n].name), "%s", comm);
        snprintf(tmp[n].cmd, sizeof(tmp[n].cmd), "%s", cmd[0] ? cmd : comm);
        snprintf(tmp[n].scope, sizeof(tmp[n].scope), "%s", scope);
        snprintf(tmp[n].state, sizeof(tmp[n].state), "%c", st ? st : '?');
        tmp[n].rss = rss;
        tmp[n].cpu = cpu;
        n++;
    }
    closedir(d);

    for (int i = 1; i < n; i++) {
        proc_t key = tmp[i];
        int j = i - 1;
        while (j >= 0 && tmp[j].rss < key.rss) { tmp[j + 1] = tmp[j]; j--; }
        tmp[j + 1] = key;
    }
    int keep = n < 1024 ? n : 1024;
    for (int i = 0; i < keep; i++) s->proc[i] = tmp[i];
    s->nproc = keep;

    for (int i = 0; i < np && i < 1024; i++) {
        c->prev_pids[i] = pids[i];
        c->prev_ticks[i] = ticks[i];
    }
    c->nprev = np;
    c->nsorted = np < 1024 ? np : 1024;
    memcpy(c->sorted_pids, c->prev_pids, sizeof(int) * (size_t)c->nsorted);
    qsort(c->sorted_pids, (size_t)c->nsorted, sizeof(int), pid_cmp);
}

/* cheap: readdir only, no file reads; detects new/gone pids within one tick */
static int proc_set_changed(collector_t *c)
{
    DIR *d = opendir("/proc");
    if (!d) return 1;
    struct dirent *e;
    int n = 0, changed = 0;
    while ((e = readdir(d))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        int pid = atoi(e->d_name);
        n++;
        if (c->nsorted == 0 ||
            !bsearch(&pid, c->sorted_pids, (size_t)c->nsorted, sizeof(int), pid_cmp)) {
            changed = 1;
            break;
        }
    }
    closedir(d);
    if (n != c->nsorted) changed = 1;
    return changed;
}

static void read_usb(snapshot_t *s)
{
    DIR *d = opendir("/sys/bus/usb/devices");
    if (!d) { s->nusb = 0; return; }
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d)) && n < 16) {
        if (e->d_name[0] == '.') continue;
        char path[300], vid[16] = {0}, pid[16] = {0};
        snprintf(path, sizeof(path), "/sys/bus/usb/devices/%s/idVendor", e->d_name);
        if (read_file(path, vid, sizeof(vid)) < 0) continue;
        snprintf(path, sizeof(path), "/sys/bus/usb/devices/%s/idProduct", e->d_name);
        if (read_file(path, pid, sizeof(pid)) < 0) continue;
        vid[strcspn(vid, "\n")] = 0;
        pid[strcspn(pid, "\n")] = 0;
        if (!strcmp(vid, "1d6b")) continue;   /* root hubs */

        char name[64] = {0};
        snprintf(path, sizeof(path), "/sys/bus/usb/devices/%s/product", e->d_name);
        if (read_file(path, name, sizeof(name)) < 0 || name[0] == 0) {
            snprintf(path, sizeof(path), "/sys/bus/usb/devices/%s/manufacturer", e->d_name);
            read_file(path, name, sizeof(name));
        }
        if (name[0] == 0) snprintf(name, sizeof(name), "USB %s:%s", vid, pid);
        name[strcspn(name, "\n")] = 0;

        snprintf(s->usb[n].vid, sizeof(s->usb[n].vid), "%s", vid);
        snprintf(s->usb[n].pid, sizeof(s->usb[n].pid), "%s", pid);
        snprintf(s->usb[n].name, sizeof(s->usb[n].name), "%s", name);
        snprintf(s->usb[n].bus, sizeof(s->usb[n].bus), "%s", e->d_name);
        n++;
    }
    closedir(d);
    s->nusb = n;
}

static void read_drm(snapshot_t *s)
{
    DIR *d = opendir("/sys/class/drm");
    if (!d) { s->ndrm = 0; return; }
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d)) && n < 8) {
        if (strncmp(e->d_name, "card", 4) || !strchr(e->d_name, '-')) continue;
        char path[300], st[24] = {0};
        snprintf(path, sizeof(path), "/sys/class/drm/%s/status", e->d_name);
        if (read_file(path, st, sizeof(st)) < 0) continue;
        st[strcspn(st, "\n")] = 0;
        snprintf(s->drm[n].name, sizeof(s->drm[n].name), "%s", e->d_name);
        snprintf(s->drm[n].status, sizeof(s->drm[n].status), "%s", st);
        n++;
    }
    closedir(d);
    s->ndrm = n;
}

static void read_pci(snapshot_t *s)
{
    DIR *d = opendir("/sys/bus/pci/devices");
    if (!d) { s->npci = 0; return; }
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d))) if (e->d_name[0] != '.') n++;
    closedir(d);
    s->npci = n;
}

void collect_tick(collector_t *c, snapshot_t *s)
{
    double ts = now_ts();
    double dt = ts - c->prev_ts;
    if (dt <= 0 || dt > 60) dt = 1.0;

    s->ts = ts;
    s->epoch = (unsigned long long)time(NULL);
    read_mem(s);
    read_cpu(c, s);
    read_net(c, s, dt);
    read_disk(c, s, dt);
    read_temps(c, s);
    read_battery(s);
    read_psi(s);
    read_fs(s);

    double now = now_ts();
    int interval = c->proc_interval > 0 ? c->proc_interval : 3000;
    if (c->last_proc_scan <= 0 || (now - c->last_proc_scan) * 1000.0 >= interval) {
        double pdt = c->last_proc_scan > 0 ? now - c->last_proc_scan : dt;
        read_procs(c, s, pdt);
        c->last_proc_scan = now;
        c->nproc_cache = s->nproc;
        memcpy(c->proc_cache, s->proc, sizeof(proc_t) * (size_t)s->nproc);
    } else {
        s->nproc = c->nproc_cache;
        memcpy(s->proc, c->proc_cache, sizeof(proc_t) * (size_t)c->nproc_cache);
    }

    if (c->last_dev_scan <= 0 || now - c->last_dev_scan > 10.0) {
        read_usb(s);
        read_drm(s);
        read_pci(s);
        c->nusb_cache = s->nusb;
        memcpy(c->usb_cache, s->usb, sizeof(usbdev_t) * (size_t)s->nusb);
        c->ndrm_cache = s->ndrm;
        memcpy(c->drm_cache, s->drm, sizeof(drmdev_t) * (size_t)s->ndrm);
        c->npci_cache = s->npci;
        c->last_dev_scan = now;
    } else {
        s->nusb = c->nusb_cache;
        memcpy(s->usb, c->usb_cache, sizeof(usbdev_t) * (size_t)c->nusb_cache);
        s->ndrm = c->ndrm_cache;
        memcpy(s->drm, c->drm_cache, sizeof(drmdev_t) * (size_t)c->ndrm_cache);
        s->npci = c->npci_cache;
    }

    char buf[128];
    if (read_file("/proc/uptime", buf, sizeof(buf)) > 0)
        s->uptime = (unsigned long long)atof(buf);

    c->prev_ts = ts;
}

void collect_init(collector_t *c, snapshot_t *s)
{
    memset(c, 0, sizeof(*c));
    memset(s, 0, sizeof(*s));
    read_hostinfo(s);

    c->proc_interval = 5000;
    c->prev_ts = now_ts();
    collect_tick(c, s);   /* primes previous counters */
    c->inited = 1;
    struct timespec t = { 0, 150 * 1000000L };
    nanosleep(&t, NULL);
    collect_tick(c, s);   /* first real values */
}
