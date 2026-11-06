#ifndef SYSMON_STATE_H
#define SYSMON_STATE_H

#define MAX_CORES 64
#define MAX_NET 12
#define MAX_DISK 12
#define MAX_TEMP 32
#define JSON_MAX 32768

typedef struct {
    char name[16];
    unsigned long long rx, tx;
    double rx_bps, tx_bps;
} netdev_t;

typedef struct {
    char name[24];
    unsigned long long rd, wr;
    double rbps, wbps;
} diskdev_t;

typedef struct {
    char chip[32];
    char label[40];
    double c;
} temp_t;

typedef struct {
    int pid, ppid;
    unsigned uid;
    char user[16];
    char name[32];
    char cmd[128];
    char scope[64];
    char state[4];
    unsigned long long rss;
    double cpu;
} proc_t;

typedef struct {
    char vid[8], pid[8];
    char name[64];
    char bus[16];
} usbdev_t;

typedef struct {
    char name[32];
    char status[16];
} drmdev_t;

#define MAX_APPS 256
typedef struct {
    char key[96];
    char app[48];
    char display[64];
    int pids[256];
    int npid;
    double cpu;
    unsigned long long rss;
} app_t;

typedef struct {
    double ts;
    unsigned long long epoch;
    unsigned long long uptime;

    unsigned long long mem_total, mem_avail, mem_free, mem_cached, mem_buffers;
    unsigned long long mem_sreclaim, mem_shmem, mem_dirty, mem_writeback;
    unsigned long long mem_anon, mem_slab, mem_pagetables;
    unsigned long long swap_total, swap_free;

    double cpu_pct, load1, load5, load15;
    int ncores;
    double core[MAX_CORES];

    int nnet;
    netdev_t net[MAX_NET];

    int ndisk;
    diskdev_t disk[MAX_DISK];

    int ntemp;
    temp_t temp[MAX_TEMP];

    int has_batt;
    double batt_pct, batt_w;
    char batt_status[24];

    double psi_some10, psi_full10;

    int nproc;
    proc_t proc[1024];

    int nusb;
    usbdev_t usb[16];
    int npci;
    int ndrm;
    drmdev_t drm[8];

    unsigned long long fs_total, fs_free;

    char host[64], kernel[64], distro[96], cpu_model[96];
    int cpu_threads;
} snapshot_t;

typedef struct {
    unsigned long long prev_total[MAX_CORES + 1], prev_idle[MAX_CORES + 1];
    unsigned long long prev_rx[MAX_NET], prev_tx[MAX_NET];
    unsigned long long prev_rd[MAX_DISK], prev_wr[MAX_DISK];
    double prev_ts;
    int inited;

    char net_names[MAX_NET][16];
    char disk_names[MAX_DISK][24];
    int nnet, ndisk;

    int prev_pids[1024];
    unsigned long long prev_ticks[1024];
    int nprev;

    double last_proc_scan;
    int proc_interval;      /* ms */
    int sorted_pids[1024];
    int nsorted;
    proc_t proc_cache[1024];
    int nproc_cache;

    char temp_path[MAX_TEMP][256];
    char temp_label[MAX_TEMP][40];
    char temp_chip[MAX_TEMP][32];
    int ntemp_paths;
    double last_temp_enum;

    double last_dev_scan;
    int nusb_cache;
    usbdev_t usb_cache[16];
    int ndrm_cache;
    drmdev_t drm_cache[8];
    int npci_cache;
} collector_t;

void collect_init(collector_t *c, snapshot_t *s);
void collect_tick(collector_t *c, snapshot_t *s);
int json_snapshot(const snapshot_t *s, char *out, int cap);

#endif
