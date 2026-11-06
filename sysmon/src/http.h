#ifndef SYSMON_HTTP_H
#define SYSMON_HTTP_H

typedef struct {
    double t, cpu, mem, rx, tx, tmp;
} hpoint_t;

int  server_start(int port);
void server_set_procs(const proc_t *p, int n);
void server_push_history(double t, double cpu, double mem, double rx, double tx, double tmp);
void server_publish(const char *json, int len);
void server_stop(void);

#endif
