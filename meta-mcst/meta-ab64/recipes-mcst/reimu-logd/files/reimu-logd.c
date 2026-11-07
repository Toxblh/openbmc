/*
 * reimu-logd: the BMC's persistent event log and the diagnostic archive.
 *
 * The journal lives in RAM (a week of it, Storage=volatile: nothing to the NOR
 * flash). This follows it with a cursor (kept in /run) and appends to
 * /var/lib/reimu-logd/events.log on the rwfs what must survive a power loss:
 * priority <= warning, and every line of the daemons named below; kernel
 * oops/panic lines and the like count as critical. See ev-core.c for the
 * limits. At every BMC boot, one line: why it started (reset registers, read
 * only; the previous run's clean stop).
 *
 *   service   xyz.openbmc_project.Reimu.Logs
 *   object    /xyz/openbmc_project/reimu/logs
 *     method Read(t offset, u length) -> s   the event log (the old file, the
 *            current one, what's not written yet), JSON {ok,size,data(base64)}
 *     method DiagStart(b with_host) -> s     runs /usr/libexec/reimu-diag
 *     method DiagStatus() -> s               {state: idle|running|done|failed,
 *            step, done, total, size, name, error}
 *     method DiagDownload(t offset, u length) -> s   as Read, the archive
 *     method DiagDelete() -> s
 *   object    /xyz/openbmc_project/reimu/journal
 *   interface xyz.openbmc_project.Reimu.Journal
 *     method Read(s cursor, i count) -> s     a window of the BMC journal for
 *            the web UI, JSON (reimu-journal.c); count < 0: before the cursor
 * The archive goes after a download (DiagDelete) or after REIMU_DIAG_TTL s
 * (900), so that it can't fill /run. Through bmcweb the methods are POSTs,
 * which take the Administrator's privileges.
 *
 *   reimu-logd --decode-reset BOOTSTATUS SCU3C CLEAN CMDLINE
 *   reimu-logd --journal [n [cursor]]   the web UI's window, printed
 *
 * Copyright (c) 2026 MCST REIMU contributors. Licensed under GPLv2.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <syslog.h>

#include <systemd/sd-bus.h>
#include <systemd/sd-daemon.h>
#include <systemd/sd-journal.h>

#include "ev-core.h"
#include "rbuf.h"
#include "reimu-journal.h"

#define LOGDIR  "/var/lib/reimu-logd"
#define LOGFILE LOGDIR "/events.log"
#define RUNDIR  "/run/reimu-logd"
#define DIAGDIR "/run/reimu-diag"
#define DIAG    "/usr/libexec/reimu-diag"
#define SERVICE "xyz.openbmc_project.Reimu.Logs"
#define OBJ     "/xyz/openbmc_project/reimu/logs"
#define CHUNK   (1u << 20)

/* every line of these, up to info */
static const char *const SOURCES[] = { "overheatd", "reimu-fand", "reimu-hostlink", "reimu-ab", "reimu-abd", "reimu-pns-flash",
                                       "update-host-flash", NULL };

static struct ev E;
static sd_journal *J;
static char s_cursor[512];
static int s_cursor_dirty;
static sd_bus *s_bus;

static int64_t mono(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}

static void put_run(const char *name, const char *text)
{
    char p[128], tmp[136];
    snprintf(p, sizeof(p), RUNDIR "/%s", name);
    snprintf(tmp, sizeof(tmp), "%s.tmp", p);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return;
    ssize_t w = write(fd, text, strlen(text));
    close(fd);
    if (w >= 0) rename(tmp, p);
}

static void flush_now(void)
{
    if (ev_flush(&E, mono()))
    {
        static int said;
        if (!said++) fprintf(stderr, "can't write %s: %s\n", LOGFILE, strerror(errno));
    }
    if (s_cursor_dirty && !E.npend)
    {
        put_run("cursor", s_cursor);
        s_cursor_dirty = 0;
    }
    char st[160];
    snprintf(st, sizeof(st), "writes=%lu bytes=%lu dropped=%lu errors=%lu\n", E.writes, E.bytes, E.dropped, E.errors);
    put_run("stats", st);
}

/* ------------------------------------------------------------------ boot record */
static unsigned read_reg(unsigned long addr)
{
    unsigned v = ~0u;
    int fd = open("/dev/mem", O_RDONLY | O_SYNC | O_CLOEXEC);
    if (fd < 0) return v;
    long ps = sysconf(_SC_PAGESIZE);
    void *m = mmap(NULL, ps, PROT_READ, MAP_SHARED, fd, addr & ~(ps - 1));
    close(fd);
    if (m == MAP_FAILED) return v;
    v = *(volatile unsigned *)((char *)m + (addr & (ps - 1)));
    munmap(m, ps);
    return v;
}

static void read_text(const char *path, char *b, size_t n)
{
    b[0] = 0;
    FILE *f = fopen(path, "re");
    if (!f) return;
    size_t r = fread(b, 1, n - 1, f);
    b[r] = 0;
    fclose(f);
    b[strcspn(b, "\n")] = 0;
}

static void boot_record(void)
{
    if (!access(RUNDIR "/booted", F_OK)) return; /* a restart of this daemon */
    char bs[32], cmd[1024], ab[80], ver[120] = "", line[256];
    read_text("/sys/class/watchdog/watchdog0/bootstatus", bs, sizeof(bs));
    read_text("/proc/cmdline", cmd, sizeof(cmd));
    read_text("/run/initramfs/reimu-ab", ab, sizeof(ab));
    FILE *f = fopen("/etc/os-release", "re");
    if (f)
    {
        char l[160];
        while (fgets(l, sizeof(l), f))
            if (!strncmp(l, "VERSION_ID=", 11)) { l[strcspn(l, "\n")] = 0; snprintf(ver, sizeof(ver), "%s", l + 11); }
        fclose(f);
    }
    unsigned bootstatus = strtoul(bs, NULL, 0), scu = read_reg(0x1e6e203c), wdt = read_reg(0x1e785010);
    int clean = ev_prev_clean(LOGFILE);
    snprintf(line, sizeof(line), "boot: reset=%s prev=%s bootstatus=0x%x SCU3C=0x%08x WDT1=0x%08x slot=%s fw=%s",
             ev_reset_reason(bootstatus, scu, clean, cmd), clean ? "clean" : "unclean", bootstatus, scu, wdt, ab, ver);
    sd_journal_send("MESSAGE=%s", line, "PRIORITY=%d", LOG_NOTICE, "SYSLOG_IDENTIFIER=reimu-logd", NULL);
    ev_add_note(&E, time(NULL), LOG_NOTICE, line);
    put_run("booted", "1\n");
}

/* ------------------------------------------------------------------ journal */
static int field(const char *name, char *out, size_t n)
{
    const void *d;
    size_t l, nl = strlen(name) + 1;
    out[0] = 0;
    if (sd_journal_get_data(J, name, &d, &l) < 0 || l < nl) return -1;
    l -= nl;
    if (l >= n) l = n - 1;
    memcpy(out, (const char *)d + nl, l);
    out[l] = 0;
    return 0;
}

static int journal_open(void)
{
    int r = sd_journal_open(&J, SD_JOURNAL_LOCAL_ONLY | SD_JOURNAL_SYSTEM);
    if (r < 0) return r;
    sd_journal_set_data_threshold(J, 1024);
    char m[64];
    for (int p = 0; p <= 4 && r >= 0; p++)
    {
        snprintf(m, sizeof(m), "PRIORITY=%d", p);
        r = sd_journal_add_match(J, m, 0);
    }
    for (int i = 0; SOURCES[i] && r >= 0; i++)
    {
        r = sd_journal_add_disjunction(J);
        snprintf(m, sizeof(m), "SYSLOG_IDENTIFIER=%s", SOURCES[i]);
        if (r >= 0) r = sd_journal_add_match(J, m, 0);
    }
    if (r < 0) return r;
    char cur[sizeof(s_cursor)];
    read_text(RUNDIR "/cursor", cur, sizeof(cur));
    if (cur[0] && sd_journal_seek_cursor(J, cur) >= 0)
    {
        /* seek lands on the cursor's entry: it's done */
        if (sd_journal_next(J) > 0 && sd_journal_test_cursor(J, cur) <= 0) sd_journal_previous(J);
    }
    else sd_journal_seek_head(J);
    return 0;
}

static int is_critical(int prio, const char *id, const char *msg)
{
    if (prio <= 2) return 1;
    if (!strcmp(id, "kernel") && (strstr(msg, "Oops") || strstr(msg, "BUG:") || strstr(msg, "panic") || strstr(msg, "Unable to handle")))
        return 1;
    if (!strcmp(id, "overheatd") && (strstr(msg, "verheat") || strstr(msg, "emergency"))) return 1;
    return 0;
}

static void journal_read(void)
{
    int64_t now = mono();
    for (int i = 0; i < 2000; i++)
    {
        int r = sd_journal_next(J);
        if (r <= 0) break;
        char pr[8], id[64], pids[16], msg[EV_LINE_MAX + 16];
        uint64_t usec = 0;
        if (field("PRIORITY", pr, sizeof(pr))) continue;
        int prio = atoi(pr);
        if (field("MESSAGE", msg, sizeof(msg))) continue;
        if (field("SYSLOG_IDENTIFIER", id, sizeof(id)) && field("_COMM", id, sizeof(id))) snprintf(id, sizeof(id), "?");
        field("_PID", pids, sizeof(pids));
        sd_journal_get_realtime_usec(J, &usec);
        char *c = NULL;
        if (sd_journal_get_cursor(J, &c) >= 0 && c)
        {
            snprintf(s_cursor, sizeof(s_cursor), "%s", c);
            free(c);
            s_cursor_dirty = 1;
        }
        if (prio > 6) continue;
        ev_add(&E, now, (time_t)(usec / 1000000), prio, id, atoi(pids), msg, is_critical(prio, id, msg));
    }
}

/* ------------------------------------------------------------------ chunks */
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *chunk_json(const unsigned char *d, size_t n, uint64_t size)
{
    size_t cap = n / 3 * 4 + 128;
    char *out = malloc(cap);
    if (!out) return NULL;
    size_t o = (size_t)snprintf(out, cap, "{\"ok\":true,\"size\":%llu,\"data\":\"", (unsigned long long)size);
    for (size_t i = 0; i < n; i += 3)
    {
        unsigned v = d[i] << 16 | (i + 1 < n ? d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        out[o++] = B64[v >> 18 & 63];
        out[o++] = B64[v >> 12 & 63];
        out[o++] = i + 1 < n ? B64[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? B64[v & 63] : '=';
    }
    memcpy(out + o, "\"}", 3);
    return out;
}

/* a slice of a part of a sequence of files: returns what it copied */
static size_t slice(const char *path, uint64_t *off, unsigned char *b, size_t n, uint64_t *total)
{
    struct stat st;
    if (stat(path, &st)) return 0;
    uint64_t sz = st.st_size;
    *total += sz;
    size_t got = 0;
    if (*off < sz && n)
    {
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd >= 0)
        {
            ssize_t r = pread(fd, b, n < sz - *off ? n : sz - *off, *off);
            if (r > 0) got = r;
            close(fd);
        }
        *off = 0;
    }
    else *off -= *off < sz ? *off : sz;
    return got;
}

static int reply_err(sd_bus_message *m, const char *text)
{
    char b[256];
    snprintf(b, sizeof(b), "{\"ok\":false,\"error\":\"%s\"}", text);
    return sd_bus_reply_method_return(m, "s", b);
}

static int m_read(sd_bus_message *m, void *ud, sd_bus_error *err)
{
    uint64_t off;
    uint32_t len;
    int r = sd_bus_message_read(m, "tu", &off, &len);
    if (r < 0) return r;
    if (len > CHUNK) len = CHUNK;
    unsigned char *b = malloc(len ? len : 1);
    if (!b) return -ENOMEM;
    uint64_t total = 0, o = off;
    size_t got = slice(LOGFILE ".1", &o, b, len, &total);
    /* the rest in the current file, from its start when the slice went past .1 */
    size_t g2 = slice(LOGFILE, &o, b + got, len - got, &total);
    got += g2;
    uint64_t pend = E.npend;
    if (o < pend && got < len)
    {
        size_t k = len - got < pend - o ? len - got : pend - o;
        memcpy(b + got, E.pend + o, k);
        got += k;
    }
    total += pend;
    char *j = chunk_json(b, got, total);
    free(b);
    if (!j) return -ENOMEM;
    r = sd_bus_reply_method_return(m, "s", j);
    free(j);
    return r;
}

/* ------------------------------------------------------------------ diag */
static pid_t s_diag;
static int s_diag_failed;
static char s_diag_err[160];
static char s_diag_file[200];
static int64_t s_diag_done;
static long s_ttl = 900;

static void diag_file_clear(void)
{
    if (s_diag_file[0]) unlink(s_diag_file);
    s_diag_file[0] = 0;
    s_diag_done = 0;
    s_diag_failed = 0;
    s_diag_err[0] = 0;
}

static void diag_reap(void)
{
    if (!s_diag) return;
    int st;
    if (waitpid(s_diag, &st, WNOHANG) != s_diag) return;
    s_diag = 0;
    s_diag_done = mono();
    char out[300];
    read_text(RUNDIR "/diag.out", out, sizeof(out));
    if (WIFEXITED(st) && !WEXITSTATUS(st) && out[0] == '/') snprintf(s_diag_file, sizeof(s_diag_file), "%s", out);
    else
    {
        s_diag_failed = 1;
        snprintf(s_diag_err, sizeof(s_diag_err), "%s", WIFEXITED(st) && WEXITSTATUS(st) == 3 ? "уже выполняется" : "reimu-diag не завершился");
    }
    sd_notify(0, "STATUS=diag done");
}

/* archives older than their time, a shell's too */
static void diag_sweep(void)
{
    if (s_diag) return;
    DIR *d = opendir(DIAGDIR);
    if (!d) return;
    struct dirent *e;
    time_t now = time(NULL);
    char p[300];
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    while ((e = readdir(d)))
    {
        size_t l = strlen(e->d_name);
        if (l < 7 || strcmp(e->d_name + l - 7, ".tar.gz")) continue;
        snprintf(p, sizeof(p), DIAGDIR "/%s", e->d_name);
        struct stat st;
        if (stat(p, &st)) continue;
        int own = !strcmp(p, s_diag_file);
        /* a clock set at boot makes mtimes useless: our own by the monotonic clock */
        if (own ? mono() - s_diag_done >= s_ttl : (now - st.st_mtime >= s_ttl || now < st.st_mtime))
        {
            unlink(p);
            if (own) diag_file_clear();
        }
    }
    closedir(d);
}

static int m_diag_start(sd_bus_message *m, void *ud, sd_bus_error *err)
{
    int host = 0;
    int r = sd_bus_message_read(m, "b", &host);
    if (r < 0) return r;
    diag_reap();
    if (s_diag) return sd_bus_reply_method_return(m, "s", "{\"ok\":true,\"state\":\"running\"}");
    diag_file_clear();
    pid_t p = fork();
    if (p < 0) return reply_err(m, "fork");
    if (!p)
    {
        int fd = open(RUNDIR "/diag.out", O_WRONLY | O_CREAT | O_TRUNC, 0600);
        int nul = open("/dev/null", O_RDONLY);
        if (fd >= 0) dup2(fd, 1);
        if (nul >= 0) dup2(nul, 0);
        sigset_t all;
        sigemptyset(&all);
        sigprocmask(SIG_SETMASK, &all, NULL);
        setsid();
        if (host) execl(DIAG, DIAG, "--with-host", NULL);
        else execl(DIAG, DIAG, NULL);
        _exit(127);
    }
    s_diag = p;
    return sd_bus_reply_method_return(m, "s", "{\"ok\":true,\"state\":\"running\"}");
}

static int m_diag_status(sd_bus_message *m, void *ud, sd_bus_error *err)
{
    diag_reap();
    char j[600], step[120] = "";
    int done = 0, total = 0;
    if (s_diag)
    {
        char l[160];
        read_text(DIAGDIR "/progress", l, sizeof(l));
        sscanf(l, "%d %d %119s", &done, &total, step);
        snprintf(j, sizeof(j), "{\"ok\":true,\"state\":\"running\",\"step\":\"%s\",\"done\":%d,\"total\":%d}", step, done, total);
    }
    else if (s_diag_failed) snprintf(j, sizeof(j), "{\"ok\":true,\"state\":\"failed\",\"error\":\"%s\"}", s_diag_err);
    else if (s_diag_file[0])
    {
        struct stat st;
        const char *b = strrchr(s_diag_file, '/');
        if (stat(s_diag_file, &st)) snprintf(j, sizeof(j), "{\"ok\":true,\"state\":\"idle\"}");
        else snprintf(j, sizeof(j), "{\"ok\":true,\"state\":\"done\",\"name\":\"%s\",\"size\":%lld,\"left\":%ld}", b ? b + 1 : s_diag_file,
                      (long long)st.st_size, s_ttl - (mono() - s_diag_done));
    }
    else snprintf(j, sizeof(j), "{\"ok\":true,\"state\":\"idle\"}");
    return sd_bus_reply_method_return(m, "s", j);
}

static int m_diag_download(sd_bus_message *m, void *ud, sd_bus_error *err)
{
    uint64_t off;
    uint32_t len;
    int r = sd_bus_message_read(m, "tu", &off, &len);
    if (r < 0) return r;
    diag_reap();
    if (!s_diag_file[0]) return reply_err(m, "no archive");
    if (len > CHUNK) len = CHUNK;
    unsigned char *b = malloc(len ? len : 1);
    if (!b) return -ENOMEM;
    uint64_t total = 0, o = off;
    size_t got = slice(s_diag_file, &o, b, len, &total);
    char *j = chunk_json(b, got, total);
    free(b);
    if (!j) return -ENOMEM;
    r = sd_bus_reply_method_return(m, "s", j);
    free(j);
    return r;
}

static int m_diag_delete(sd_bus_message *m, void *ud, sd_bus_error *err)
{
    diag_reap();
    if (!s_diag) diag_file_clear();
    return sd_bus_reply_method_return(m, "s", "{\"ok\":true}");
}

static const sd_bus_vtable vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Read", "tu", "s", m_read, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("DiagStart", "b", "s", m_diag_start, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("DiagStatus", "", "s", m_diag_status, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("DiagDownload", "tu", "s", m_diag_download, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("DiagDelete", "", "s", m_diag_delete, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END
};

static int m_journal(sd_bus_message *m, void *ud, sd_bus_error *err)
{
    const char *cur;
    int32_t count;
    int r = sd_bus_message_read(m, "si", &cur, &count);
    if (r < 0) return r;
    struct buf b = { 0 };
    journal_json(&b, cur, count);
    r = sd_bus_reply_method_return(m, "s", b.s ? b.s : "{}");
    free(b.s);
    return r;
}

static const sd_bus_vtable journal_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Read", "si", "s", m_journal, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END
};

/* ------------------------------------------------------------------ main */
static int shutting_down(void)
{
    /* a stop of this unit alone isn't a stop of the BMC */
    return !system("systemctl list-jobs --no-legend --no-pager 2>/dev/null | grep -qE '(shutdown|reboot|poweroff|kexec|halt)\\.target'");
}

int main(int argc, char **argv)
{
    if (argc == 6 && !strcmp(argv[1], "--decode-reset"))
    {
        puts(ev_reset_reason(strtoul(argv[2], NULL, 0), strtoul(argv[3], NULL, 0), atoi(argv[4]), argv[5]));
        return 0;
    }
    if (argc >= 2 && argc <= 4 && !strcmp(argv[1], "--journal"))
    {
        struct buf b = { 0 };
        journal_json(&b, argc > 3 ? argv[3] : "", argc > 2 ? atoi(argv[2]) : -20);
        if (b.s) puts(b.s);
        free(b.s);
        return 0;
    }
    if (argc != 1)
    {
        fprintf(stderr, "usage: %s [--decode-reset BOOTSTATUS SCU3C CLEAN CMDLINE | --journal [n [cursor]]]\n", argv[0]);
        return 2;
    }
    const char *t = getenv("REIMU_DIAG_TTL");
    if (t && atol(t) > 0) s_ttl = atol(t);
    mkdir(RUNDIR, 0700);
    mkdir(DIAGDIR, 0700);
    chmod(DIAGDIR, 0700);
    if (mkdir(LOGDIR, 0750) && errno != EEXIST) fprintf(stderr, "%s: %s\n", LOGDIR, strerror(errno));
    ev_init(&E, LOGFILE);
    boot_record();
    int r = journal_open();
    if (r < 0)
    {
        fprintf(stderr, "journal: %s\n", strerror(-r));
        return 1;
    }
    r = sd_bus_default_system(&s_bus);
    if (r >= 0) r = sd_bus_add_object_vtable(s_bus, NULL, OBJ, SERVICE, vtable, NULL);
    if (r >= 0)
        r = sd_bus_add_object_vtable(s_bus, NULL, "/xyz/openbmc_project/reimu/journal", "xyz.openbmc_project.Reimu.Journal",
                                     journal_vtable, NULL);
    if (r >= 0) r = sd_bus_add_object_manager(s_bus, NULL, "/");
    if (r >= 0) r = sd_bus_request_name(s_bus, SERVICE, 0);
    if (r < 0)
    {
        fprintf(stderr, "D-Bus: %s\n", strerror(-r));
        return 1;
    }
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGINT);
    sigprocmask(SIG_BLOCK, &set, NULL);
    int sfd = signalfd(-1, &set, SFD_CLOEXEC);
    sd_notify(0, "READY=1");
    int64_t last_sweep = 0, last_watchdog = 0;
    for (;;)
    {
        journal_read();
        int64_t now = mono();
        if (ev_flush_due(&E, now)) flush_now();
        diag_reap();
        if (now - last_sweep >= 10)
        {
            last_sweep = now;
            diag_sweep();
        }
        if (now - last_watchdog >= 15)
        {
            last_watchdog = now;
            sd_notify(0, "WATCHDOG=1");
        }
        int in = ev_flush_in(&E, now);
        int to = 5000;
        if (in >= 0 && in * 1000 < to) to = in * 1000 + 10;
        if (s_diag) to = to > 1000 ? 1000 : to;
        struct pollfd p[3] = { { sd_journal_get_fd(J), sd_journal_get_events(J), 0 },
                               { sd_bus_get_fd(s_bus), sd_bus_get_events(s_bus), 0 },
                               { sfd, POLLIN, 0 } };
        poll(p, 3, to);
        sd_journal_process(J);
        while ((r = sd_bus_process(s_bus, NULL)) > 0) { }
        if (r < 0) break;
        sd_bus_flush(s_bus);
        if (p[2].revents & POLLIN) break;
    }
    journal_read();
    int shut = shutting_down();
    if (shut) ev_add_note(&E, time(NULL), LOG_NOTICE, EV_STOP_TEXT);
    flush_now();
    sd_bus_flush_close_unref(s_bus);
    return 0;
}
