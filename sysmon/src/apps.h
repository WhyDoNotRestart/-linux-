#ifndef SYSMON_APPS_H
#define SYSMON_APPS_H

#include "state.h"

void apps_init(void);
const char *apps_lookup(const char *app);
const char *apps_display(const char *app);
int apps_group(const proc_t *procs, int n, app_t *out, int max);

#endif
