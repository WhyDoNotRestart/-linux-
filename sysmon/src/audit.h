#ifndef SYSMON_AUDIT_H
#define SYSMON_AUDIT_H

void audit_init(const char *path);
const char *audit_path(void);
void audit_log(const char *fmt, ...);

#endif
