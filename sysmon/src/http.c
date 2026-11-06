#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <signal.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include "state.h"
#include "http.h"
#include "audit.h"
#include "apps.h"
#include "assets.h"

#define MAX_CLIENTS 32

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cond = PTHREAD_COND_INITIALIZER;
static char g_json[JSON_MAX];
static int  g_len;
static unsigned long g_gen;
static int  g_clients;
static hpoint_t g_hist[120];
static int g_hist_n, g_hist_pos;
static struct timespec g_start;
static proc_t g_procs[1024];
static int g_nprocs;
static unsigned long g_requests;
static volatile int g_run = 1;
static int  g_listen = -1;

void server_publish(const char *json, int len)
{
    if (len <= 0 || len >= JSON_MAX) return;
    pthread_mutex_lock(&g_lock);
    memcpy(g_json, json, (size_t)len);
    g_json[len] = 0;
    g_len = len;
    g_gen++;
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_lock);
}

void server_push_history(double t, double cpu, double mem, double rx, double tx, double tmp)
{
    pthread_mutex_lock(&g_lock);
    g_hist[g_hist_pos].t = t;
    g_hist[g_hist_pos].cpu = cpu;
    g_hist[g_hist_pos].mem = mem;
    g_hist[g_hist_pos].rx = rx;
    g_hist[g_hist_pos].tx = tx;
    g_hist[g_hist_pos].tmp = tmp;
    g_hist_pos = (g_hist_pos + 1) % 120;
    if (g_hist_n < 120) g_hist_n++;
    pthread_mutex_unlock(&g_lock);
}

static int history_json(char *out, int cap)
{
    int o = 0;
    o += snprintf(out + o, cap - o, "{\"series\":[");
    pthread_mutex_lock(&g_lock);
    int start = (g_hist_pos - g_hist_n + 120) % 120;
    for (int i = 0; i < g_hist_n && o < cap - 128; i++) {
        hpoint_t *p = &g_hist[(start + i) % 120];
        o += snprintf(out + o, cap - o,
                      "%s{\"t\":%.0f,\"cpu\":%.1f,\"mem\":%.1f,\"rx\":%.0f,\"tx\":%.0f,\"tmp\":%.1f}",
                      i ? "," : "", p->t, p->cpu, p->mem, p->rx, p->tx, p->tmp);
    }
    pthread_mutex_unlock(&g_lock);
    o += snprintf(out + o, cap - o, "]}");
    return o;
}

void server_set_procs(const proc_t *p, int n)
{
    if (n > 1024) n = 1024;
    pthread_mutex_lock(&g_lock);
    memcpy(g_procs, p, sizeof(proc_t) * (size_t)n);
    g_nprocs = n;
    pthread_mutex_unlock(&g_lock);
}

static void jstr(char *dst, size_t cap, const char *src)
{
    size_t o = 0;
    for (; *src && o + 8 < cap; src++) {
        unsigned char ch = (unsigned char)*src;
        if (ch == '"' || ch == '\\') {
            dst[o++] = '\\';
            dst[o++] = (char)ch;
        } else if (ch < 0x20) {
            o += (size_t)snprintf(dst + o, cap - o, "\\u%04x", ch);
        } else {
            dst[o++] = (char)ch;
        }
    }
    dst[o] = 0;
}

static int proc_by_cpu(const void *a, const void *b)
{
    const proc_t *x = a, *y = b;
    return x->cpu < y->cpu ? 1 : x->cpu > y->cpu ? -1 : 0;
}

static int proc_by_rss(const void *a, const void *b)
{
    const proc_t *x = a, *y = b;
    return x->rss < y->rss ? 1 : x->rss > y->rss ? -1 : 0;
}

static int procs_json(char *out, int cap, const char *sort, int limit, const char *q,
                   int *total_out, int *matched_out)
{
    proc_t tmp[1024], list[1024];
    pthread_mutex_lock(&g_lock);
    int n = g_nprocs;
    memcpy(tmp, g_procs, sizeof(proc_t) * (size_t)n);
    pthread_mutex_unlock(&g_lock);

    int total = n, matched = 0;
    for (int i = 0; i < n; i++) {
        if (q && *q) {
            char hay[256];
            snprintf(hay, sizeof(hay), "%s %s %s", tmp[i].name, tmp[i].cmd, tmp[i].user);
            if (!strcasestr(hay, q)) continue;
        }
        list[matched++] = tmp[i];
    }
    *total_out = total;
    *matched_out = matched;
    n = matched;

    qsort(list, (size_t)n, sizeof(proc_t), !strcmp(sort, "cpu") ? proc_by_cpu : proc_by_rss);
    if (limit <= 0 || limit > n) limit = n;

    int o = 0;
    o += snprintf(out + o, cap - o, "{\"procs\":[");
    for (int i = 0; i < limit; i++) {
        char ent[1024], ecmd[640], ename[128], escope[192], euser[64];
        jstr(ecmd, sizeof(ecmd), list[i].cmd);
        jstr(ename, sizeof(ename), list[i].name);
        jstr(escope, sizeof(escope), list[i].scope);
        jstr(euser, sizeof(euser), list[i].user);
        int el = snprintf(ent, sizeof(ent),
                          "%s{\"pid\":%d,\"ppid\":%d,\"uid\":%u,\"user\":\"%s\","
                          "\"name\":\"%s\",\"cmd\":\"%s\",\"scope\":\"%s\",\"state\":\"%s\",\"rss\":%llu,\"cpu\":%.1f}",
                          i ? "," : "", list[i].pid, list[i].ppid, list[i].uid, euser,
                          ename, ecmd, escope, list[i].state, list[i].rss, list[i].cpu);
        if (o + el > cap - 64) break;
        memcpy(out + o, ent, (size_t)el);
        o += el;
    }
    if (o > cap - 64) o = cap - 64;
    o += snprintf(out + o, cap - o, "],\"total\":%d,\"matched\":%d}", total, matched);
    return o;
}

static int read_file_static(const char *path, char *buf, size_t cap)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, cap - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = 0;
    return (int)n;
}

static int is_ancestor(int pid)
{
    int cur = getpid();
    for (int i = 0; i < 12; i++) {
        if (cur <= 1) break;
        if (cur == pid) return 1;
        char path[64], buf[512];
        snprintf(path, sizeof(path), "/proc/%d/stat", cur);
        if (read_file_static(path, buf, sizeof(buf)) < 0) break;
        char *rp = strrchr(buf, ')');
        if (!rp) break;
        int ppid = 0;
        if (sscanf(rp + 1, " %*c %d", &ppid) != 1) break;
        cur = ppid;
    }
    return 0;
}

static int send_all(int fd, const char *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, buf + off, len - off, MSG_NOSIGNAL);
        if (n <= 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

static void header(int fd, int code, const char *type, long len, const char *extra)
{
    char h[512];
    const char *reason = code == 200 ? "OK" : code == 404 ? "Not Found" : "Error";
    int n = snprintf(h, sizeof(h),
                     "HTTP/1.1 %d %s\r\n"
                     "Server: sysmon\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %ld\r\n"
                     "Cache-Control: no-store\r\n"
                     "Connection: close\r\n"
                     "%s"
                     "\r\n", code, reason, type, len, extra ? extra : "");
    send_all(fd, h, (size_t)n);
}

static int health_json(char *out, int cap)
{
    FILE *f = fopen("/proc/self/status", "r");
    long rss = 0, thr = 0;
    char line[256];
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            if (!strncmp(line, "VmRSS:", 6)) rss = atol(line + 6);
            if (!strncmp(line, "Threads:", 8)) thr = atol(line + 8);
        }
        fclose(f);
    }
    struct rusage ru;
    double cpu_pct = 0, elapsed = 0;
    if (getrusage(RUSAGE_SELF, &ru) == 0) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        elapsed = (now.tv_sec - g_start.tv_sec) + (now.tv_nsec - g_start.tv_nsec) / 1e9;
        double cpu = ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 +
                     ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
        cpu_pct = elapsed > 0 ? cpu * 100.0 / elapsed : 0;
    }
    const char *ap = audit_path();
    return snprintf(out, cap,
                    "{\"ok\":true,\"rss_kb\":%ld,\"threads\":%ld,\"json_bytes\":%d,"
                    "\"uptime_s\":%.0f,\"cpu_pct\":%.2f,\"audit\":\"%s\",\"requests\":%lu}",
                    rss, thr, g_len, elapsed, cpu_pct, ap ? ap : "stderr", g_requests);
}

static void sse_stream(int fd)
{
    unsigned long last = 0;
    char chunk[64];
    const char *hello = "retry: 2000\n: connected\n\n";
    int n = snprintf(chunk, sizeof(chunk), "%x\r\n", (unsigned)strlen(hello));
    send_all(fd, chunk, (size_t)n);
    send_all(fd, hello, strlen(hello));
    send_all(fd, "\r\n", 2);

    for (; g_run; ) {
        char copy[JSON_MAX];
        pthread_mutex_lock(&g_lock);
        while (g_run && g_gen == last) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 10;
            if (pthread_cond_timedwait(&g_cond, &g_lock, &ts) == ETIMEDOUT) break;
        }
        if (!g_run) { pthread_mutex_unlock(&g_lock); break; }
        if (g_gen == last) {
            pthread_mutex_unlock(&g_lock);
            const char *ping = ": ping\n\n";
            snprintf(chunk, sizeof(chunk), "%x\r\n", (unsigned)strlen(ping));
            if (send_all(fd, chunk, strlen(chunk)) < 0) break;
            if (send_all(fd, ping, strlen(ping)) < 0) break;
            if (send_all(fd, "\r\n", 2) < 0) break;
            continue;
        }
        last = g_gen;
        memcpy(copy, g_json, (size_t)g_len + 1);
        int len = g_len;
        pthread_mutex_unlock(&g_lock);

        char msg[JSON_MAX + 16];
        int mlen = snprintf(msg, sizeof(msg), "event: tick\ndata: %s\n\n", copy);
        snprintf(chunk, sizeof(chunk), "%x\r\n", (unsigned)mlen);
        if (send_all(fd, chunk, strlen(chunk)) < 0) break;
        if (send_all(fd, msg, (size_t)mlen) < 0) break;
        if (send_all(fd, "\r\n", 2) < 0) break;
        (void)len;
    }
}

static void json_reply(int fd, int code, const char *body)
{
    header(fd, code, "application/json", (long)strlen(body), NULL);
    send_all(fd, body, strlen(body));
}

static void peer_addr(int fd, char *out, size_t cap)
{
    struct sockaddr_in sa;
    socklen_t sl = sizeof(sa);
    if (getpeername(fd, (struct sockaddr *)&sa, &sl) == 0)
        snprintf(out, cap, "%s", inet_ntoa(sa.sin_addr));
    else
        snprintf(out, cap, "-");
}

static void kill_err(int fd, int pid, const char *msg)
{
    char out[256];
    snprintf(out, sizeof(out), "{\"ok\":false,\"error\":\"%s\"}", msg);
    json_reply(fd, 400, out);
    audit_log("kill refused pid=%d reason=%s", pid, msg);
}

static void handle_kill(int fd, const char *req)
{
    const char *body = strstr(req, "\r\n\r\n");
    body = body ? body + 4 : req;

    int pid = 0;
    char sig_name[8] = "TERM";
    const char *p = strstr(body, "\"pid\"");
    if (p) { const char *c = strchr(p, ':'); if (c) pid = atoi(c + 1); }
    const char *sp = strstr(body, "\"signal\"");
    if (sp) {
        const char *c = strchr(sp, ':');
        if (c) { const char *q = strchr(c, '"'); if (q) sscanf(q + 1, "%7[A-Za-z]", sig_name); }
    }

    if (pid <= 1) { kill_err(fd, pid, "refused: pid 1"); return; }
    if (pid == getpid()) { kill_err(fd, pid, "refused: sysmon itself"); return; }
    if (pid == getppid()) { kill_err(fd, pid, "refused: parent process"); return; }
    if (is_ancestor(pid)) { kill_err(fd, pid, "refused: ancestor process"); return; }

    char ppath[64];
    struct stat sb;
    snprintf(ppath, sizeof(ppath), "/proc/%d", pid);
    if (stat(ppath, &sb) < 0) { kill_err(fd, pid, "no such process"); return; }
    if (sb.st_uid != getuid()) { kill_err(fd, pid, "permission denied: other user"); return; }

    int sig = 0;
    if (!strcasecmp(sig_name, "TERM")) sig = SIGTERM;
    else if (!strcasecmp(sig_name, "KILL")) sig = SIGKILL;
    else if (!strcasecmp(sig_name, "INT")) sig = SIGINT;
    else if (!strcasecmp(sig_name, "HUP")) sig = SIGHUP;
    else { kill_err(fd, pid, "unsupported signal"); return; }

    char comm[64] = {0};
    snprintf(ppath, sizeof(ppath), "/proc/%d/comm", pid);
    read_file_static(ppath, comm, sizeof(comm));
    comm[strcspn(comm, "\n")] = 0;

    char peer[32];
    peer_addr(fd, peer, sizeof(peer));

    if (kill(pid, sig) < 0) {
        char msg[128];
        snprintf(msg, sizeof(msg), "kill failed: %s", strerror(errno));
        kill_err(fd, pid, msg);
        audit_log("kill failed pid=%d name=%s signal=%s peer=%s err=%s",
                  pid, comm, sig_name, peer, strerror(errno));
        return;
    }

    audit_log("kill ok pid=%d uid=%u name=%s signal=%s peer=%s",
              pid, (unsigned)sb.st_uid, comm, sig_name, peer);
    fprintf(stderr, "sysmon: sent SIG%s to pid %d\n", sig_name, pid);
    char out[128];
    snprintf(out, sizeof(out), "{\"ok\":true,\"pid\":%d,\"signal\":\"%s\"}", pid, sig_name);
    json_reply(fd, 200, out);
}

static int apps_json(char *out, int cap)
{
    app_t list[MAX_APPS];
    pthread_mutex_lock(&g_lock);
    int n = apps_group(g_procs, g_nprocs, list, MAX_APPS);
    pthread_mutex_unlock(&g_lock);

    int o = 0;
    o += snprintf(out + o, cap - o, "{\"apps\":[");
    for (int i = 0; i < n; i++) {
        char ent[768], ekey[192], eapp[128], edisp[192];
        jstr(ekey, sizeof(ekey), list[i].key);
        jstr(eapp, sizeof(eapp), list[i].app);
        jstr(edisp, sizeof(edisp), list[i].display);
        int el = snprintf(ent, sizeof(ent),
                          "%s{\"key\":\"%s\",\"app\":\"%s\",\"display\":\"%s\","
                          "\"count\":%d,\"cpu\":%.1f,\"rss\":%llu}",
                          i ? "," : "", ekey, eapp, edisp,
                          list[i].npid, list[i].cpu, list[i].rss);
        if (o + el > cap - 64) break;
        memcpy(out + o, ent, (size_t)el);
        o += el;
    }
    if (o > cap - 64) o = cap - 64;
    o += snprintf(out + o, cap - o, "],\"total\":%d}", n);
    return o;
}

static void handle_kill_app(int fd, const char *req)
{
    const char *body = strstr(req, "\r\n\r\n");
    body = body ? body + 4 : req;

    char key[96] = {0}, appname[64] = {0};
    int want_pid = 0;

    const char *k = strstr(body, "\"key\"");
    if (k) { const char *c = strchr(k, ':'); if (c) { const char *q = strchr(c, '"'); if (q) sscanf(q + 1, "%95[^\"]", key); } }
    const char *a = strstr(body, "\"app\"");
    if (a) { const char *c = strchr(a, ':'); if (c) { const char *q = strchr(c, '"'); if (q) sscanf(q + 1, "%63[^\"]", appname); } }
    const char *p = strstr(body, "\"pid\"");
    if (p) { const char *c = strchr(p, ':'); if (c) want_pid = atoi(c + 1); }


    app_t list[MAX_APPS];
    pthread_mutex_lock(&g_lock);
    int n = apps_group(g_procs, g_nprocs, list, MAX_APPS);
    int idx = -1;
    for (int i = 0; i < n; i++) {
        if (key[0] && !strcmp(list[i].key, key)) { idx = i; break; }
        if (!key[0] && appname[0] && !strcmp(list[i].app, appname)) { idx = i; break; }
        if (!key[0] && !appname[0] && want_pid) {
            for (int j = 0; j < list[i].npid; j++)
                if (list[i].pids[j] == want_pid) { idx = i; break; }
            if (idx >= 0) break;
        }
    }
    if (idx < 0) {
        pthread_mutex_unlock(&g_lock);
        /* 兜底：按单个进程结束 */
        if (want_pid > 1 && !is_ancestor(want_pid) && want_pid != getpid()) {
            char pp[64];
            struct stat sb;
            snprintf(pp, sizeof(pp), "/proc/%d", want_pid);
            if (stat(pp, &sb) == 0 && sb.st_uid == getuid() && kill(want_pid, SIGTERM) == 0) {
                audit_log("kill-app fallback pid=%d killed=1", want_pid);
                json_reply(fd, 200, "{\"ok\":true,\"killed\":1,\"failed\":0}");
                return;
            }
        }
        kill_err(fd, want_pid, "app not found");
        return;
    }

    app_t target = list[idx];
    pthread_mutex_unlock(&g_lock);

    int killed = 0, failed = 0;
    for (int i = 0; i < target.npid; i++) {
        int pid = target.pids[i];
        if (pid <= 1 || pid == getpid() || is_ancestor(pid)) { failed++; continue; }
        char ppath[64];
        struct stat sb;
        snprintf(ppath, sizeof(ppath), "/proc/%d", pid);
        if (stat(ppath, &sb) < 0 || sb.st_uid != getuid()) { failed++; continue; }
        if (kill(pid, SIGTERM) == 0) killed++;
        else failed++;
    }

    char peer[32];
    peer_addr(fd, peer, sizeof(peer));
    audit_log("kill-app app=%s killed=%d failed=%d peer=%s", target.app, killed, failed, peer);

    char out[256];
    snprintf(out, sizeof(out),
             "{\"ok\":%s,\"app\":\"%s\",\"killed\":%d,\"failed\":%d}",
             killed > 0 ? "true" : "false", target.app, killed, failed);
    json_reply(fd, killed > 0 ? 200 : 400, out);
}

static void *client_thread(void *arg)
{
    int fd = (int)(long)arg;
    pthread_mutex_lock(&g_lock);
    g_requests++;
    pthread_mutex_unlock(&g_lock);
    char req[4096];
    size_t used = 0;

    while (used < sizeof(req) - 1) {
        ssize_t n = recv(fd, req + used, sizeof(req) - 1 - used, 0);
        if (n <= 0) break;
        used += (size_t)n;
        req[used] = 0;
        char *hdr_end = strstr(req, "\r\n\r\n");
        if (hdr_end) {
            size_t body_have = used - (size_t)(hdr_end + 4 - req);
            int want = 0;
            const char *cl = strcasestr(req, "content-length:");
            if (cl) want = atoi(cl + 15);
            if ((int)body_have >= want) break;
        }
    }

    char method[8] = {0}, path[256] = {0};
    if (sscanf(req, "%7s %255s", method, path) != 2) {
        header(fd, 404, "text/plain", 9, NULL);
        send_all(fd, "not found", 9);
        goto done;
    }

    char *qs = strchr(path, '?');
    if (qs) { *qs = 0; qs++; }

    if (!strcmp(method, "POST")) {
        if (!strcmp(path, "/api/v1/kill")) handle_kill(fd, req);
        else if (!strcmp(path, "/api/v1/kill-app")) handle_kill_app(fd, req);
        else {
            header(fd, 404, "text/plain", 9, NULL);
            send_all(fd, "not found", 9);
        }
        goto done;
    }
    if (strcmp(method, "GET")) {
        header(fd, 405, "text/plain", 11, NULL);
        send_all(fd, "bad method", 10);
        goto done;
    }

    if (!strcmp(path, "/api/v1/apps")) {
        static char abuf[JSON_MAX];
        int n = apps_json(abuf, sizeof(abuf));
        header(fd, 200, "application/json", n, NULL);
        send_all(fd, abuf, (size_t)n);
    } else if (!strcmp(path, "/api/v1/procs")) {
        static char pbuf[JSON_MAX];
        static char qbuf[128];
        const char *sort = "rss";
        int limit = 20;
        if (qs && strstr(qs, "sort=cpu")) sort = "cpu";
        if (qs) { const char *l = strstr(qs, "limit="); if (l) limit = atoi(l + 6); }

        qbuf[0] = 0;
        if (qs) {
            const char *q = strstr(qs, "q=");
            if (q) {
                q += 2;
                size_t j = 0;
                while (*q && *q != '&' && j + 1 < sizeof(qbuf)) {
                    if (*q == '%' && q[1] && q[2]) {
                        int hi = q[1] <= '9' ? q[1] - '0' : (q[1] | 32) - 'a' + 10;
                        int lo = q[2] <= '9' ? q[2] - '0' : (q[2] | 32) - 'a' + 10;
                        qbuf[j++] = (char)((hi << 4) | lo);
                        q += 3;
                    } else if (*q == '+') {
                        qbuf[j++] = ' ';
                        q++;
                    } else {
                        qbuf[j++] = *q++;
                    }
                }
                qbuf[j] = 0;
            }
        }
        int total = 0, matched = 0;
        int n = procs_json(pbuf, sizeof(pbuf), sort, limit, qbuf, &total, &matched);
        header(fd, 200, "application/json", n, NULL);
        send_all(fd, pbuf, (size_t)n);
    } else if (!strcmp(path, "/") || !strcmp(path, "/index.html")) {
        header(fd, 200, "text/html; charset=utf-8", (long)index_html_len, NULL);
        send_all(fd, (const char *)index_html, index_html_len);
    } else if (!strcmp(path, "/api/v1/snapshot")) {
        char copy[JSON_MAX];
        pthread_mutex_lock(&g_lock);
        int len = g_len;
        if (len > 0) memcpy(copy, g_json, (size_t)len);
        pthread_mutex_unlock(&g_lock);
        if (!len) { header(fd, 200, "application/json", 2, NULL); send_all(fd, "{}", 2); }
        else {
            header(fd, 200, "application/json", len, NULL);
            send_all(fd, copy, (size_t)len);
        }
    } else if (!strcmp(path, "/api/v1/stream")) {
        char h[512];
        int n = snprintf(h, sizeof(h),
                         "HTTP/1.1 200 OK\r\n"
                         "Server: sysmon\r\n"
                         "Content-Type: text/event-stream\r\n"
                         "Cache-Control: no-store\r\n"
                         "Connection: keep-alive\r\n"
                         "Transfer-Encoding: chunked\r\n"
                         "\r\n");
        send_all(fd, h, (size_t)n);
        sse_stream(fd);
    } else if (!strcmp(path, "/api/v1/history")) {
        static char hbuf[JSON_MAX];
        int n = history_json(hbuf, sizeof(hbuf));
        header(fd, 200, "application/json", n, NULL);
        send_all(fd, hbuf, (size_t)n);
    } else if (!strcmp(path, "/healthz")) {
        char buf[256];
        int n = health_json(buf, sizeof(buf));
        header(fd, 200, "application/json", n, NULL);
        send_all(fd, buf, (size_t)n);
    } else {
        header(fd, 404, "text/plain", 9, NULL);
        send_all(fd, "not found", 9);
    }

done:
    close(fd);
    pthread_mutex_lock(&g_lock);
    g_clients--;
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

static void *accept_loop(void *arg)
{
    (void)arg;
    struct sockaddr_in sa;
    socklen_t sl = sizeof(sa);

    for (; g_run; ) {
        int c = accept(g_listen, (struct sockaddr *)&sa, &sl);
        if (c < 0) {
            if (errno == EINTR) continue;
            break;
        }
        int one = 1;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        struct timeval tv = { 2, 0 };
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        pthread_mutex_lock(&g_lock);
        if (g_clients >= MAX_CLIENTS) {
            pthread_mutex_unlock(&g_lock);
            close(c);
            continue;
        }
        g_clients++;
        pthread_mutex_unlock(&g_lock);

        pthread_t th;
        if (pthread_create(&th, NULL, client_thread, (void *)(long)c) == 0)
            pthread_detach(th);
        else {
            close(c);
            pthread_mutex_lock(&g_lock);
            g_clients--;
            pthread_mutex_unlock(&g_lock);
        }
    }
    return NULL;
}

int server_start(int port)
{
    clock_gettime(CLOCK_MONOTONIC, &g_start);
    g_listen = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen < 0) return -1;

    int one = 1;
    setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(g_listen, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(g_listen);
        g_listen = -1;
        return -1;
    }
    if (listen(g_listen, 16) < 0) {
        close(g_listen);
        g_listen = -1;
        return -1;
    }

    pthread_t th;
    if (pthread_create(&th, NULL, accept_loop, NULL) != 0) return -1;
    pthread_detach(th);
    return 0;
}

void server_stop(void)
{
    g_run = 0;
    if (g_listen >= 0) {
        shutdown(g_listen, SHUT_RDWR);
        close(g_listen);
        g_listen = -1;
    }
    pthread_mutex_lock(&g_lock);
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_lock);
}
