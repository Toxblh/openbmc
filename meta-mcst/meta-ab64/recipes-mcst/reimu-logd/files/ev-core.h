/* reimu-logd: the event log's logic, without systemd (native tests). */
#ifndef EV_CORE_H
#define EV_CORE_H
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define EV_FILE_MAX   (256 * 1024)  /* a file; two kept: 512 KiB and a batch */
#define EV_MIN_BYTES  4096          /* a minute's events */
#define EV_HOUR_BYTES (32 * 1024)   /* an hour's (a flood can't erase the log in hours) */
#define EV_CRIT_HOUR  (64 * 1024)   /* critical lines pass the above up to this */
#define EV_SRC_LINES  20            /* lines a minute of one source */
#define EV_FLUSH_S    60            /* a flush at most this often... */
#define EV_CRIT_S     5             /* ...but critical lines after this */
#define EV_LINE_MAX   300
#define EV_PENDING    (32 * 1024)
#define EV_NSRC       16

struct ev
{
    char path[200];                 /* the log; .1 the previous one */
    char pend[EV_PENDING];
    size_t npend;
    int crit_pending;
    int64_t last_flush;             /* s, monotonic; 0: none yet */
    int64_t min_start, hour_start;
    size_t min_bytes, hour_bytes, crit_bytes;
    struct { uint32_t h; int n; } src[EV_NSRC];
    uint32_t last_hash;             /* the previous line, its repeats */
    time_t last_when;
    unsigned rep;
    unsigned dropped_now;           /* since the last note */
    int checked_end;                /* a torn last line looked after */
    /* stats */
    unsigned long writes, bytes, dropped, errors;
};

void ev_init(struct ev *e, const char *path);
/* a journal entry; returns 1 if taken into the pending batch */
int ev_add(struct ev *e, int64_t now, time_t when, int prio, const char *id, int pid, const char *msg, int critical);
/* a line that always goes in (boot record, the stop line), flushed at the next ev_flush_due */
void ev_add_note(struct ev *e, time_t when, int prio, const char *msg);
int ev_flush_due(const struct ev *e, int64_t now);
/* writes the batch (one write, fsync, rotation): 0 ok, -1 kept for the next try */
int ev_flush(struct ev *e, int64_t now);
/* seconds to the next flush, -1: nothing pending */
int ev_flush_in(const struct ev *e, int64_t now);
/* did the previous run end in a clean stop? (the log's last line) */
int ev_prev_clean(const char *path);
#define EV_STOP_TEXT "stopping: system shutdown"
/* why the BMC started: reset registers, the clean stop, the command line */
const char *ev_reset_reason(unsigned bootstatus, unsigned scu3c, int clean, const char *cmdline);
#endif
