#define _GNU_SOURCE
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include "audit.h"

#define AUDIT_MAX_BYTES (1024 * 1024)

static FILE *g_file;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static char g_path[512];

static void open_log(void)
{
    g_file = fopen(g_path, "a");
    if (g_file) setvbuf(g_file, NULL, _IOLBF, 0);
}

static void rotate_if_needed(void)
{
    if (!g_file || !g_path[0]) return;
    struct stat st;
    if (stat(g_path, &st) != 0 || st.st_size < AUDIT_MAX_BYTES) return;

    fclose(g_file);
    g_file = NULL;

    char old[600];
    snprintf(old, sizeof(old), "%s.1", g_path);
    unlink(old);
    rename(g_path, old);
    open_log();
}

void audit_init(const char *path)
{
    if (!path || !*path) return;
    snprintf(g_path, sizeof(g_path), "%s", path);
    open_log();
}

const char *audit_path(void)
{
    return g_file ? g_path : NULL;
}

void audit_log(const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    char ts[40];
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S%z", &tm);

    pthread_mutex_lock(&g_mu);
    rotate_if_needed();
    if (g_file) {
        fprintf(g_file, "%s %s\n", ts, msg);
        fflush(g_file);
    } else {
        fprintf(stderr, "sysmon: %s %s\n", ts, msg);
    }
    pthread_mutex_unlock(&g_mu);
}
