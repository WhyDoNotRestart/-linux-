#define _GNU_SOURCE
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include "state.h"

static void j_esc(char *out, int cap, int *off, const char *s)
{
    for (; *s && *off < cap - 8; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') {
            out[(*off)++] = '\\';
            out[(*off)++] = (char)ch;
        } else if (ch < 0x20) {
            *off += snprintf(out + *off, cap - *off, "\\u%04x", ch);
        } else {
            out[(*off)++] = (char)ch;
        }
    }
}

static void j_add(char *out, int cap, int *off, const char *fmt, ...)
{
    if (*off >= cap - 1) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out + *off, cap - *off, fmt, ap);
    va_end(ap);
    if (n > 0) *off += n;
    if (*off > cap - 1) *off = cap - 1;
}

int json_snapshot(const snapshot_t *s, char *out, int cap)
{
    int o = 0;
    if (cap < 256) return 0;

    j_add(out, cap, &o, "{\"ts\":%.3f,\"epoch\":%llu,\"uptime\":%llu,", s->ts, s->epoch, s->uptime);

    j_add(out, cap, &o, "\"host\":{\"name\":\"");
    j_esc(out, cap, &o, s->host);
    j_add(out, cap, &o, "\",\"kernel\":\"");
    j_esc(out, cap, &o, s->kernel);
    j_add(out, cap, &o, "\",\"distro\":\"");
    j_esc(out, cap, &o, s->distro);
    j_add(out, cap, &o, "\",\"cpu\":\"");
    j_esc(out, cap, &o, s->cpu_model);
    j_add(out, cap, &o, "\",\"threads\":%d},", s->ncores);

    j_add(out, cap, &o,
          "\"mem\":{\"total\":%llu,\"avail\":%llu,\"free\":%llu,\"cached\":%llu,"
          "\"buffers\":%llu,\"sreclaim\":%llu,\"shmem\":%llu,\"anon\":%llu,"
          "\"slab\":%llu,\"pagetables\":%llu,\"swap_total\":%llu,\"swap_free\":%llu},",
          s->mem_total, s->mem_avail, s->mem_free, s->mem_cached,
          s->mem_buffers, s->mem_sreclaim, s->mem_shmem, s->mem_anon,
          s->mem_slab, s->mem_pagetables, s->swap_total, s->swap_free);

    j_add(out, cap, &o, "\"cpu\":{\"pct\":%.1f,\"load1\":%.2f,\"load5\":%.2f,\"load15\":%.2f,\"cores\":[",
          s->cpu_pct, s->load1, s->load5, s->load15);
    for (int i = 0; i < s->ncores; i++)
        j_add(out, cap, &o, "%s%.1f", i ? "," : "", s->core[i]);
    j_add(out, cap, &o, "]},");

    j_add(out, cap, &o, "\"net\":[");
    for (int i = 0; i < s->nnet; i++)
        j_add(out, cap, &o, "%s{\"name\":\"%s\",\"rx\":%llu,\"tx\":%llu,\"rx_bps\":%.0f,\"tx_bps\":%.0f}",
              i ? "," : "", s->net[i].name, s->net[i].rx, s->net[i].tx,
              s->net[i].rx_bps, s->net[i].tx_bps);
    j_add(out, cap, &o, "],");

    j_add(out, cap, &o, "\"disk\":[");
    for (int i = 0; i < s->ndisk; i++)
        j_add(out, cap, &o, "%s{\"name\":\"%s\",\"rd\":%llu,\"wr\":%llu,\"rbps\":%.0f,\"wbps\":%.0f}",
              i ? "," : "", s->disk[i].name, s->disk[i].rd, s->disk[i].wr,
              s->disk[i].rbps, s->disk[i].wbps);
    j_add(out, cap, &o, "],");

    j_add(out, cap, &o, "\"temp\":[");
    for (int i = 0; i < s->ntemp; i++) {
        j_add(out, cap, &o, "%s{\"chip\":\"", i ? "," : "");
        j_esc(out, cap, &o, s->temp[i].chip);
        j_add(out, cap, &o, "\",\"label\":\"");
        j_esc(out, cap, &o, s->temp[i].label);
        j_add(out, cap, &o, "\",\"c\":%.1f}", s->temp[i].c);
    }
    j_add(out, cap, &o, "],");

    j_add(out, cap, &o,
          "\"batt\":{\"present\":%d,\"pct\":%.0f,\"status\":\"%s\",\"w\":%.2f},",
          s->has_batt, s->batt_pct, s->batt_status, s->batt_w);

    j_add(out, cap, &o, "\"psi\":{\"some10\":%.2f,\"full10\":%.2f},", s->psi_some10, s->psi_full10);

    j_add(out, cap, &o, "\"fs\":{\"total\":%llu,\"free\":%llu},", s->fs_total, s->fs_free);

    j_add(out, cap, &o, "\"usb\":[");
    for (int i = 0; i < s->nusb; i++) {
        j_add(out, cap, &o, "%s{\"vid\":\"%s\",\"pid\":\"%s\",\"name\":\"",
              i ? "," : "", s->usb[i].vid, s->usb[i].pid);
        j_esc(out, cap, &o, s->usb[i].name);
        j_add(out, cap, &o, "\",\"bus\":\"%s\"}", s->usb[i].bus);
    }
    j_add(out, cap, &o, "],\"pci\":%d,\"drm\":[", s->npci);
    for (int i = 0; i < s->ndrm; i++)
        j_add(out, cap, &o, "%s{\"name\":\"%s\",\"status\":\"%s\"}",
              i ? "," : "", s->drm[i].name, s->drm[i].status);
    j_add(out, cap, &o, "],");

    j_add(out, cap, &o, "\"procs\":[");
    int np = s->nproc < 12 ? s->nproc : 12;
    for (int i = 0; i < np; i++) {
        j_add(out, cap, &o, "%s{\"pid\":%d,\"ppid\":%d,\"uid\":%u,\"user\":\"%s\",\"name\":\"",
              i ? "," : "", s->proc[i].pid, s->proc[i].ppid, s->proc[i].uid, s->proc[i].user);
        j_esc(out, cap, &o, s->proc[i].name);
        j_add(out, cap, &o, "\",\"cmd\":\"");
        j_esc(out, cap, &o, s->proc[i].cmd);
        j_add(out, cap, &o, "\",\"scope\":\"");
        j_esc(out, cap, &o, s->proc[i].scope);
        j_add(out, cap, &o, "\",\"state\":\"%s\",\"rss\":%llu,\"cpu\":%.1f}",
              s->proc[i].state, s->proc[i].rss, s->proc[i].cpu);
    }
    j_add(out, cap, &o, "]}");

    out[o] = 0;
    return o;
}
