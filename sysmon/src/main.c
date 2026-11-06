#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>
#include "state.h"
#include "http.h"
#include "audit.h"
#include "apps.h"

static void mkdir_parents(const char *path)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    char *slash = strrchr(tmp, '/');
    if (!slash) return;
    *slash = 0;

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

static const char *default_log_path(void)
{
    static char path[512];
    const char *xdg = getenv("XDG_STATE_HOME");
    const char *home = getenv("HOME");
    if (xdg && *xdg) snprintf(path, sizeof(path), "%s/sysmon/kill.log", xdg);
    else if (home && *home) snprintf(path, sizeof(path), "%s/.local/state/sysmon/kill.log", home);
    else return NULL;
    return path;
}

static void push_history(const snapshot_t *s)
{
    double used = (double)(s->mem_total - s->mem_avail);
    double mem_pct = s->mem_total ? used * 100.0 / (double)s->mem_total : 0;
    double rx = 0, tx = 0, tmax = 0;
    for (int i = 0; i < s->nnet; i++) { rx += s->net[i].rx_bps; tx += s->net[i].tx_bps; }
    for (int i = 0; i < s->ntemp; i++) if (s->temp[i].c > tmax) tmax = s->temp[i].c;
    server_push_history((double)s->epoch, s->cpu_pct, mem_pct, rx, tx, tmax);
}

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig) { (void)sig; g_stop = 1; }

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) < 0) {}
}

int main(int argc, char **argv)
{
    int port = 8787;
    int interval = 1000;
    int proc_interval = 5000;
    int once = 0;
    const char *log_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--interval") && i + 1 < argc) interval = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--proc-interval") && i + 1 < argc) proc_interval = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--log") && i + 1 < argc) log_path = argv[++i];
        else if (!strcmp(argv[i], "--once")) once = 1;
        else if (!strcmp(argv[i], "--help")) {
            printf("usage: sysmon [--port N] [--interval MS] [--proc-interval MS] [--log FILE] [--once]\n");
            return 0;
        }
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    collector_t col;
    snapshot_t snap;
    char json[JSON_MAX];

    collect_init(&col, &snap);
    apps_init();
    col.proc_interval = proc_interval > 0 ? proc_interval : 3000;

    if (!log_path) log_path = default_log_path();
    if (log_path) {
        mkdir_parents(log_path);
        audit_init(log_path);
    }
    json_snapshot(&snap, json, sizeof(json));

    if (once) {
        printf("%s\n", json);
        return 0;
    }

    if (server_start(port) < 0) {
        fprintf(stderr, "sysmon: cannot bind 127.0.0.1:%d\n", port);
        return 1;
    }
    fprintf(stderr, "sysmon: listening on http://127.0.0.1:%d  (interval %d ms, proc scan %d ms)\n",
            port, interval, col.proc_interval);
    fprintf(stderr, "sysmon: audit log: %s\n", audit_path() ? audit_path() : "stderr only (no writable path)");

    server_publish(json, (int)strlen(json));
    push_history(&snap);
    server_set_procs(snap.proc, snap.nproc);

    while (!g_stop) {
        sleep_ms(interval);
        if (g_stop) break;
        collect_tick(&col, &snap);
        int n = json_snapshot(&snap, json, sizeof(json));
        if (n > 0) {
            server_publish(json, n);
            push_history(&snap);
            server_set_procs(snap.proc, snap.nproc);
        }
    }

    server_stop();
    return 0;
}
