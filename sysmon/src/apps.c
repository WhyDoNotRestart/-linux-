#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <pwd.h>
#include <unistd.h>
#include "apps.h"

#define MAX_MAP 512

static struct { char exec[64]; char name[80]; } g_map[MAX_MAP];
static struct { char id[80]; char name[80]; } g_ids[MAX_MAP];
static int g_nmap, g_nids;

static const struct { const char *key; const char *name; } g_alias[] = {
    { "org.gnome.Shell", "GNOME Shell" },
    { "gnome-shell", "GNOME Shell" },
    { "microsoft-edge", "Microsoft Edge" },
    { "msedge", "Microsoft Edge" },
    { "google-chrome", "Google Chrome" },
    { "chromium", "Chromium" },
    { "firefox", "Firefox" },
    { "code", "Visual Studio Code" },
    { "wechat", "WeChat" },
    { "nautilus", "Files" },
    { "gnome-terminal", "Terminal" },
    { "ptyxis", "Terminal" },
};

static void strip_nl(char *s)
{
    s[strcspn(s, "\n")] = 0;
    size_t L = strlen(s);
    while (L && (s[L-1] == ' ' || s[L-1] == '\t')) s[--L] = 0;
}

static void add_exec(const char *exec_line, const char *name)
{
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", exec_line);
    strip_nl(tmp);

    char *save = NULL;
    char *tok = strtok_r(tmp, " ", &save);
    if (!tok) return;

    /* flatpak run ... org.mozilla.firefox */
    if (!strcmp(tok, "flatpak")) {
        char *last = NULL, *t;
        while ((t = strtok_r(NULL, " ", &save))) last = t;
        if (last && g_nids < MAX_MAP) {
            char id[80];
            snprintf(id, sizeof(id), "%s", last);
            size_t L = strlen(id);
            if (L > 8 && !strcmp(id + L - 8, ".desktop")) id[L - 8] = 0;
            snprintf(g_ids[g_nids].id, sizeof(g_ids[g_nids].id), "%s", id);
            snprintf(g_ids[g_nids].name, sizeof(g_ids[g_nids].name), "%s", name);
            g_nids++;
        }
        return;
    }
    if (!strcmp(tok, "env")) {
        while ((tok = strtok_r(NULL, " ", &save)) && strchr(tok, '=')) {}
        if (!tok) return;
    }

    const char *base = strrchr(tok, '/');
    base = base ? base + 1 : tok;
    if (g_nmap < MAX_MAP && base[0]) {
        snprintf(g_map[g_nmap].exec, sizeof(g_map[g_nmap].exec), "%s", base);
        snprintf(g_map[g_nmap].name, sizeof(g_map[g_nmap].name), "%s", name);
        g_nmap++;
    }
}

static void scan_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t L = strlen(e->d_name);
        if (L < 8 || strcmp(e->d_name + L - 8, ".desktop")) continue;

        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;

        char line[512], name[96] = {0}, exec[256] = {0};
        while (fgets(line, sizeof(line), f)) {
            if (!name[0] && !strncmp(line, "Name=", 5) && line[5] != '[') {
                snprintf(name, sizeof(name), "%s", line + 5);
                strip_nl(name);
            } else if (!exec[0] && !strncmp(line, "Exec=", 5)) {
                snprintf(exec, sizeof(exec), "%s", line + 5);
                strip_nl(exec);
            }
            if (name[0] && exec[0]) break;
        }
        fclose(f);
        if (!name[0]) continue;

        if (exec[0]) add_exec(exec, name);

        if (g_nids < MAX_MAP) {
            char id[80];
            snprintf(id, sizeof(id), "%s", e->d_name);
            id[strlen(id) - 8] = 0;
            snprintf(g_ids[g_nids].id, sizeof(g_ids[g_nids].id), "%s", id);
            snprintf(g_ids[g_nids].name, sizeof(g_ids[g_nids].name), "%s", name);
            g_nids++;
        }
    }
    closedir(d);
}

void apps_init(void)
{
    g_nmap = g_nids = 0;
    scan_dir("/usr/share/applications");
    scan_dir("/usr/local/share/applications");
    const char *home = getenv("HOME");
    if (home) {
        char p[512];
        snprintf(p, sizeof(p), "%s/.local/share/applications", home);
        scan_dir(p);
    }
}

static int ci_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

const char *apps_lookup(const char *app)
{
    if (!app || !*app) return NULL;
    for (unsigned i = 0; i < sizeof(g_alias)/sizeof(g_alias[0]); i++)
        if (ci_eq(g_alias[i].key, app)) return g_alias[i].name;
    for (int i = 0; i < g_nmap; i++)
        if (ci_eq(g_map[i].exec, app)) return g_map[i].name;
    for (int i = 0; i < g_nids; i++)
        if (ci_eq(g_ids[i].id, app)) return g_ids[i].name;
    return NULL;
}

const char *apps_display(const char *app)
{
    static char out[80];
    if (!app || !*app) return app ? app : "";

    for (int i = 0; i < g_nmap; i++)
        if (ci_eq(g_map[i].exec, app)) return g_map[i].name;
    for (int i = 0; i < g_nids; i++)
        if (ci_eq(g_ids[i].id, app)) return g_ids[i].name;

    snprintf(out, sizeof(out), "%s", app);
    out[0] = (char)toupper((unsigned char)out[0]);
    return out;
}

/* systemd 会把 desktop id 里的特殊字符转义成 \xNN */
static void unescape_unit(char *s)
{
    char *r = s, *w = s;
    while (*r) {
        if (r[0] == '\\' && r[1] == 'x' && isxdigit((unsigned char)r[2]) && isxdigit((unsigned char)r[3])) {
            int hi = isdigit((unsigned char)r[2]) ? r[2] - '0' : (tolower((unsigned char)r[2]) - 'a' + 10);
            int lo = isdigit((unsigned char)r[3]) ? r[3] - '0' : (tolower((unsigned char)r[3]) - 'a' + 10);
            *w++ = (char)((hi << 4) | lo);
            r += 4;
        } else {
            *w++ = *r++;
        }
    }
    *w = 0;
}

static void app_from_scope(const char *scope, char *out, size_t cap)
{
    out[0] = 0;
    if (!scope[0]) return;

    if (!strncmp(scope, "snap.", 5)) {
        const char *p = scope + 5;
        const char *dot = strchr(p, '.');
        snprintf(out, cap, "%.*s", (int)(dot ? dot - p : strlen(p)), p);
        return;
    }

    char tmp[128];
    snprintf(tmp, sizeof(tmp), "%s", scope);
    unescape_unit(tmp);

    char *p = strstr(tmp, "app-");
    if (p) p += 4;
    else p = tmp;

    if (!strncmp(p, "gnome-", 6)) p += 6;

    char *dot = strstr(p, ".scope");
    if (!dot) dot = strstr(p, ".service");
    if (dot) *dot = 0;

    char *at = strchr(p, '@');
    if (at) *at = 0;

    char *dash = strrchr(p, '-');
    if (dash && dash[1] >= '0' && dash[1] <= '9') *dash = 0;

    snprintf(out, cap, "%s", p);
}

static int find_app(app_t *out, int n, const char *key)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(out[i].key, key)) return i;
    return -1;
}

int apps_group(const proc_t *procs, int n, app_t *out, int max)
{
    int na = 0;
    int pids[1024], ppids[1024], np = 0;

    for (int i = 0; i < n && np < 1024; i++) {
        pids[np] = procs[i].pid;
        ppids[np] = procs[i].ppid;
        np++;
    }

    for (int i = 0; i < n; i++) {
        const proc_t *p = &procs[i];
        if (p->rss == 0 && p->ppid == 2 && p->scope[0] == 0) continue;   /* 内核线程 */
        char key[96], app[64];
        app[0] = 0;

        /* 只有 app-* 或 snap.* 这类 scope 才当作应用名，其余用 cmd 推导 */
        if (strstr(p->scope, "app-") || !strncmp(p->scope, "snap.", 5))
            app_from_scope(p->scope, app, sizeof(app));
        if (!app[0]) {
            const char *c = p->cmd[0] ? p->cmd : p->name;
            const char *sp = strchr(c, ' ');
            int L = sp ? (int)(sp - c) : (int)strlen(c);
            const char *base = c;
            for (int k = 0; k < L; k++) if (c[k] == '/') base = c + k + 1;
            snprintf(app, sizeof(app), "%.*s", (int)(c + L - base), base);
        }

        /* 先用完整 id 查桌面名（org.gnome.Shell → GNOME Shell） */
        const char *dn = apps_lookup(app);
        if (!dn) {
            char *dot = strrchr(app, '.');
            if (dot && dot[1]) dn = apps_lookup(dot + 1);
        }
        if (!dn) {
            char *dot = strrchr(app, '.');
            if (dot && dot[1]) memmove(app, dot + 1, strlen(dot + 1) + 1);
        }
        if (!dn && (strstr(app, "spawn") || strstr(app, "dsh-subprocess"))) dn = "终端会话";
        if (dn && !strcmp(dn, "终端会话")) snprintf(app, sizeof(app), "%s", "终端会话");

        if (app[0]) {
            char lower[64];
            snprintf(lower, sizeof(lower), "%s", app);
            for (char *q = lower; *q; q++) *q = (char)tolower((unsigned char)*q);
            snprintf(key, sizeof(key), "app:%s", lower);
        } else if (p->scope[0]) {
            snprintf(key, sizeof(key), "%s", p->scope);
        } else {
            int root = p->pid, guard = 0;
            for (;;) {
                int found = -1;
                for (int k = 0; k < np; k++) if (pids[k] == root) { found = ppids[k]; break; }
                if (found <= 1 || guard++ > 20) break;
                root = found;
            }
            snprintf(key, sizeof(key), "pid:%d", root);
        }

        int idx = find_app(out, na, key);
        if (idx < 0) {
            if (na >= max) continue;
            idx = na++;
            snprintf(out[idx].key, sizeof(out[idx].key), "%s", key);
            snprintf(out[idx].app, sizeof(out[idx].app), "%s", app);
            snprintf(out[idx].display, sizeof(out[idx].display), "%s",
                     dn ? dn : apps_display(app));
            out[idx].npid = 0;
            out[idx].cpu = 0;
            out[idx].rss = 0;
        }
        if (out[idx].npid < 256) out[idx].pids[out[idx].npid++] = p->pid;
        out[idx].cpu += p->cpu;
        out[idx].rss += p->rss;
    }

    for (int i = 1; i < na; i++) {
        app_t k = out[i];
        int j = i - 1;
        while (j >= 0 && out[j].rss < k.rss) { out[j + 1] = out[j]; j--; }
        out[j + 1] = k;
    }
    return na;
}
