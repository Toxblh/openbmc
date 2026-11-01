/* reimu-logd: the event log's logic. Batches lines in RAM and appends them to
 * the rwfs in one write: at most one flush a minute (critical lines: within
 * seconds), 4 KiB a minute and 32 KiB an hour, 20 lines a minute of a source,
 * repeats counted, what was dropped said. Two files of 256 KiB at most.
 * NOR: jffs2 writes through; a crash mid-write leaves at worst a torn last
 * line, ended by the next start.
 *
 * Copyright (c) 2026 MCST REIMU contributors. Licensed under GPLv2.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ev-core.h"

void ev_init(struct ev *e, const char *path)
{
    memset(e, 0, sizeof(*e));
    snprintf(e->path, sizeof(e->path), "%s", path);
}

static uint32_t hash(const char *s, uint32_t h)
{
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
    return h;
}

static void stamp(char *b, size_t n, time_t when)
{
    struct tm tm;
    gmtime_r(&when, &tm);
    strftime(b, n, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

/* a whole line into the batch; 0 when it doesn't fit */
static int put(struct ev *e, time_t when, int prio, const char *src, const char *msg)
{
    char ts[32], line[EV_LINE_MAX + 8];
    stamp(ts, sizeof(ts), when);
    int n = snprintf(line, EV_LINE_MAX, "%s %d %s: %s", ts, prio, src, msg);
    if (n >= EV_LINE_MAX) n = EV_LINE_MAX - 1;
    for (int i = 0; i < n; i++)
        if ((unsigned char)line[i] < 32) line[i] = ' ';
    line[n++] = '\n';
    if (e->npend + n > sizeof(e->pend)) return 0;
    memcpy(e->pend + e->npend, line, n);
    e->npend += n;
    return n;
}

static void end_repeats(struct ev *e)
{
    if (!e->rep) return;
    char m[64];
    snprintf(m, sizeof(m), "(the previous line %u more times)", e->rep);
    e->rep = 0;
    put(e, e->last_when, 6, "reimu-logd", m);
}

static void windows(struct ev *e, int64_t now)
{
    if (now - e->min_start >= 60)
    {
        e->min_start = now;
        e->min_bytes = 0;
        memset(e->src, 0, sizeof(e->src));
    }
    if (now - e->hour_start >= 3600)
    {
        e->hour_start = now;
        e->hour_bytes = e->crit_bytes = 0;
    }
}

int ev_add(struct ev *e, int64_t now, time_t when, int prio, const char *id, int pid, const char *msg, int critical)
{
    windows(e, now);
    uint32_t idh = hash(id, 2166136261u), h = hash(msg, idh);
    if (h == e->last_hash && !critical)
    {
        e->rep++;
        e->last_when = when;
        return 0;
    }
    size_t want = strlen(msg) + strlen(id) + 40;
    if (want > EV_LINE_MAX) want = EV_LINE_MAX;
    int ok;
    if (critical) ok = e->crit_bytes + want <= EV_CRIT_HOUR;
    else
    {
        ok = e->min_bytes + want <= EV_MIN_BYTES && e->hour_bytes + want <= EV_HOUR_BYTES;
        int slot = -1;
        for (int i = 0; i < EV_NSRC && slot < 0; i++)
            if (e->src[i].h == idh || !e->src[i].n) slot = i;
        if (slot < 0) slot = idh % EV_NSRC;
        if (ok && e->src[slot].h == idh && e->src[slot].n >= EV_SRC_LINES) ok = 0;
        if (ok)
        {
            if (e->src[slot].h != idh) e->src[slot].n = 0;
            e->src[slot].h = idh;
            e->src[slot].n++;
        }
    }
    if (!ok)
    {
        e->dropped++;
        e->dropped_now++;
        return 0;
    }
    end_repeats(e);
    char src[64];
    if (pid > 0) snprintf(src, sizeof(src), "%.48s[%d]", id, pid);
    else snprintf(src, sizeof(src), "%.48s", id);
    int n = put(e, when, prio, src, msg);
    if (!n)
    {
        e->dropped++;
        e->dropped_now++;
        return 0;
    }
    if (critical)
    {
        e->crit_bytes += n;
        e->crit_pending = 1;
    }
    else
    {
        e->min_bytes += n;
        e->hour_bytes += n;
    }
    e->last_hash = h;
    e->last_when = when;
    return 1;
}

void ev_add_note(struct ev *e, time_t when, int prio, const char *msg)
{
    end_repeats(e);
    e->last_hash = 0;
    if (put(e, when, prio, "reimu-logd", msg)) e->crit_pending = 1;
}

int ev_flush_due(const struct ev *e, int64_t now)
{
    if (!e->npend && !e->dropped_now && !e->rep) return 0;
    if (!e->last_flush) return 1;
    return now - e->last_flush >= (e->crit_pending ? EV_CRIT_S : EV_FLUSH_S);
}

int ev_flush_in(const struct ev *e, int64_t now)
{
    if (!e->npend && !e->dropped_now && !e->rep) return -1;
    if (!e->last_flush) return 0;
    int64_t left = e->last_flush + (e->crit_pending ? EV_CRIT_S : EV_FLUSH_S) - now;
    return left < 0 ? 0 : (int)left;
}

/* the previous run died in a write: its last line ends here */
static void end_torn(struct ev *e, int fd)
{
    struct stat st;
    char c = '\n';
    if (e->checked_end) return;
    e->checked_end = 1;
    if (fstat(fd, &st) || !st.st_size) return;
    if (pread(fd, &c, 1, st.st_size - 1) == 1 && c != '\n')
    {
        c = '\n';
        if (write(fd, &c, 1) < 0) e->errors++;
    }
}

int ev_flush(struct ev *e, int64_t now)
{
    end_repeats(e);
    if (e->dropped_now)
    {
        char m[80];
        snprintf(m, sizeof(m), "dropped %u events (rate limit)", e->dropped_now);
        if (put(e, time(NULL), 4, "reimu-logd", m)) e->dropped_now = 0;
    }
    if (!e->npend) return 0;
    e->last_flush = now ? now : 1;
    struct stat st;
    if (!stat(e->path, &st) && st.st_size + e->npend > EV_FILE_MAX)
    {
        char old[sizeof(e->path) + 4];
        snprintf(old, sizeof(old), "%s.1", e->path);
        if (rename(e->path, old)) e->errors++;
        e->checked_end = 0;
    }
    int fd = open(e->path, O_RDWR | O_APPEND | O_CREAT | O_CLOEXEC, 0640);
    if (fd < 0)
    {
        e->errors++;
        return -1;
    }
    end_torn(e, fd);
    ssize_t w = write(fd, e->pend, e->npend); /* one write: one jffs2 node run */
    int r = 0;
    if (w < 0 || (size_t)w != e->npend)
    {
        e->errors++;
        if (w > 0)
        {
            memmove(e->pend, e->pend + w, e->npend - w);
            e->npend -= w;
            e->checked_end = 0;
        }
        r = -1;
    }
    else
    {
        e->writes++;
        e->bytes += w;
        e->npend = 0;
        e->crit_pending = 0;
    }
    fsync(fd);
    close(fd);
    return r;
}

int ev_prev_clean(const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    struct stat st;
    char b[512];
    int clean = 0;
    if (!fstat(fd, &st) && st.st_size)
    {
        off_t o = st.st_size > (off_t)sizeof(b) - 1 ? st.st_size - (off_t)sizeof(b) + 1 : 0;
        ssize_t n = pread(fd, b, sizeof(b) - 1, o);
        if (n > 0)
        {
            b[n] = 0;
            while (n && b[n - 1] == '\n') b[--n] = 0;
            char *l = strrchr(b, '\n');
            clean = strstr(l ? l : b, EV_STOP_TEXT) != NULL;
        }
    }
    close(fd);
    return clean;
}

/* AST2500. bootstatus: the kernel's WDIOF_CARDRESET (0x20), from SCU3C's WDT1
 * bit, which it clears; a restart goes through WDT1 too, so a clean stop
 * before it tells a reboot from a real watchdog reset or a panic. SCU3C bit 0
 * (power-on) stays set until someone clears it, nobody does: alone it says
 * only that no WDT fired this time; bit 1: the external reset. A kexec
 * resets nothing: the first one from another firmware is known by its
 * command line, later ones by the clean stop without a reset. */
const char *ev_reset_reason(unsigned bootstatus, unsigned scu3c, int clean, const char *cmdline)
{
    if (cmdline && (strstr(cmdline, "reimu.ram") || strstr(cmdline, "reimu.test"))) return "kexec";
    if (bootstatus & 0x20) return clean ? "reboot" : "watchdog";
    if (scu3c & 0x2) return "external-reset";
    /* a clean stop and no reset flag: a kexec (the hardware's reset sets one;
     * QEMU's doesn't). The registers are in the record to judge by. */
    return clean ? "kexec-or-reboot" : "power-on";
}
