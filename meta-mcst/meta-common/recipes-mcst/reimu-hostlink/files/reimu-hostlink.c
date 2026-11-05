/*
 * reimu-hostlink: the BMC's end of the link with the host agent reimu-hostd
 * (meta-mcst/tools/host): the host reports its inventory and health, the BMC
 * keeps the GPU fan policy and passes a few requests (an allow-list) on.
 *
 *   service   xyz.openbmc_project.Reimu.HostLink
 *   object    /xyz/openbmc_project/reimu/hostlink
 *   interface xyz.openbmc_project.Reimu.HostLink
 *     property Json (s)       the whole state (hl_json): agent, heartbeat,
 *                             inventory, metrics, policy, events, results
 *     property Connected (b), Paired (b), Heartbeat (s: ok late lost never)
 *     method   SetGpuFan(s mode, d duty) -> s    auto | manual 20..100 %
 *     method   GpuTurbo(u minutes) -> s          100 % for 1..120 min, 0 off
 *     method   OsPower(s action) -> s            poweroff | reboot (graceful)
 *     method   LoadEdac() -> s                   modprobe e2k_edac on the host
 *     method   RequestInventory() -> s
 *     method   Pair() -> s        a one-time token (15 min) as JSON: the
 *                                 pairing string and the install command
 *     method   Unpair() -> s      the agent's key revoked
 *     method   AgentDisk() -> s   as Pair(), plus the installer's USB disk for
 *                                 the host (DISK_HELPER, reimu-agent-disk) with
 *                                 the pairing string on it; taken away when the
 *                                 agent paired and connected, or in 15 min
 *     method   AgentDiskOff() -> s
 *     method   UsbNet() -> s      the USB network with the host (USBNET_HELPER,
 *                                 reimu-usbnet status): its JSON, or {"error":..}
 *     method   UsbNetSet(s what, b on) -> s   "internet" (the host's internet
 *                                 through the BMC's port) or "enabled" (the
 *                                 USB network itself) on/off: the new status
 *   sensors   /xyz/openbmc_project/sensors/{temperature,fan_tach,fan_pwm,power}/Host_*
 *             (Sensor.Value, Availability, OperationalStatus, thresholds,
 *             the chassis association: Redfish, IPMI SDR)
 *
 * Files: /etc/reimu-hostlink.conf (KEY=VALUE, see the file), the state in
 * /var/lib/reimu-hostlink (key 0600, cert.pem 0600, policy, edac), and in
 * and for reimu-kvm a datagram to [::1]:KVM_PORT (8443) every 2 s: where the
 * host's H.264 is, or why there is none (hl_kvm_hint). In
 * /run/reimu-hostlink: gpu-temp (the GPU's junction temperature in m°C, like
 * a hwmon file, for reimu-fand; absent without a fresh value) and ctl (a
 * root-only socket: reimu-hostlink --ctl CMD).
 *
 *   reimu-hostlink                 the daemon (systemd: Type=notify, watchdog)
 *   reimu-hostlink --check-config [FILE]
 *   reimu-hostlink --ctl 'state' | 'fan manual 40' | 'turbo 5' | 'power reboot'
 *                  | 'edac' | 'inventory' | 'pair' | 'unpair' | 'disk' | 'diskoff'
 *                  | 'usbnet' | 'usbnet internet|enabled on|off'
 *                  | 'diag' (the agent's report into RUN/host-diag.b64, for reimu-diag)
 * REIMU_HOSTLINK_CONF, REIMU_HOSTLINK_DIR, REIMU_HOSTLINK_RUN: other paths
 * (tests). Built without HL_DBUS (the native tests) it has no D-Bus, writes
 * its state to RUN/state.json and takes "fandturbo SECS" on ctl.
 *
 * Copyright (c) 2026 MCST REIMU contributors. Licensed under GPLv2.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <openssl/crypto.h>

#include "hostlink.h"
#include "hl-dbus.h"

static struct hl H;
static struct hl_net *s_net;
static const char *s_conf = "/etc/reimu-hostlink.conf";
static char s_run[200] = "/run/reimu-hostlink";
static int s_ctl = -1;

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int read_text(const char *file, char *buf, size_t n)
{
    int fd = open(file, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t r = read(fd, buf, n - 1);
    close(fd);
    if (r < 0) return -1;
    buf[r] = 0;
    return 0;
}

static int load_config(struct hl_config *c, const char *file, int verbose)
{
    char text[8192] = "", err[16][128];
    hl_config_defaults(c);
    if (read_text(file, text, sizeof(text)) && errno != ENOENT) hl_log(4, "%s: %s", file, strerror(errno));
    int n = hl_config_parse(c, text, err, 16);
    for (int i = 0; i < n && i < 16; i++)
    {
        if (verbose) printf("error: %s\n", err[i]);
        else hl_log(4, "%s: %s", file, err[i]);
    }
    return n;
}

static void put_run(const char *name, const char *text)
{
    char p[300], tmp[310];
    snprintf(p, sizeof(p), "%s/%s", s_run, name);
    if (!text)
    {
        unlink(p);
        return;
    }
    snprintf(tmp, sizeof(tmp), "%s.new", p);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return;
    size_t l = strlen(text);
    int ok = write(fd, text, l) == (ssize_t)l;
    close(fd);
    if (ok) rename(tmp, p);
    else unlink(tmp);
}

/* the agent's report for reimu-diag (--ctl diag): root's only */
static void on_diag(struct hl *h, const char *b64)
{
    char p[300];
    put_run("host-diag.b64", b64);
    snprintf(p, sizeof(p), "%s/host-diag.b64", s_run);
    chmod(p, 0600);
}

/* ------------------------------------------------------------------ the installer's USB disk */
/* HELPER ARGV..., INPUT on its stdin, at most 15 s; 0 = it exited 0, else
 * its last line on stderr in err; its stdout into out (NULL: /dev/null, at most
 * 16 KiB). The main loop waits meanwhile (a disk's start can be three of these
 * in a row, WatchdogSec=30): this keeps the watchdog fed */
static int run_cmd(const char *const argv[], const char *input, struct buf *out, char *err, size_t n)
{
    int in[2], er[2], so[2] = { -1, -1 };
    const char *helper = argv[0], *action = argv[1] ? argv[1] : "";
    err[0] = 0;
    if (pipe2(in, O_CLOEXEC)) return snprintf(err, n, "pipe: %s", strerror(errno)), -1;
    if (pipe2(er, O_CLOEXEC))
    {
        close(in[0]), close(in[1]);
        return snprintf(err, n, "pipe: %s", strerror(errno)), -1;
    }
    if (out && pipe2(so, O_CLOEXEC))
    {
        close(in[0]), close(in[1]), close(er[0]), close(er[1]);
        return snprintf(err, n, "pipe: %s", strerror(errno)), -1;
    }
    pid_t pid = fork();
    if (pid < 0)
    {
        close(in[0]), close(in[1]), close(er[0]), close(er[1]);
        if (out) close(so[0]), close(so[1]);
        return snprintf(err, n, "fork: %s", strerror(errno)), -1;
    }
    if (pid == 0)
    {
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        signal(SIGPIPE, SIG_DFL);
        dup2(in[0], 0);
        dup2(er[1], 2);
        if (out) dup2(so[1], 1);
        else
        {
            int nul = open("/dev/null", O_WRONLY);
            if (nul >= 0) dup2(nul, 1);
        }
        for (int fd = 3; fd < 1024; fd++) close(fd);
        execv(helper, (char *const *)argv);
        fprintf(stderr, "%s: %s\n", helper, strerror(errno));
        _exit(127);
    }
    close(in[0]);
    close(er[1]);
    if (out) close(so[1]);
    if (input && *input && write(in[1], input, strlen(input)) < 0) { /* it went: its status says why */ }
    close(in[1]);
    char b[1024];
    size_t got = 0;
    int64_t end = now_ms() + 15000;
    int efd = er[0], ofd = out ? so[0] : -1;
    while (efd >= 0 || ofd >= 0)
    {
        int64_t left = end - now_ms();
        if (left <= 0) break;
        struct pollfd p[2] = { { efd, POLLIN, 0 }, { ofd, POLLIN, 0 } };
        int pr = poll(p, 2, left < 1000 ? (int)left : 1000);
        dbus_notify("WATCHDOG=1");
        if (pr < 0) break;
        if (pr == 0) continue;
        if (p[1].revents)
        {
            char ob[1024];
            ssize_t r = read(ofd, ob, sizeof(ob));
            if (r <= 0) close(ofd), ofd = -1;
            else if (out->len + r <= 16384) buf_add(out, ob, r);
        }
        if (p[0].revents)
        {
            ssize_t r = read(efd, b + got, sizeof(b) - 1 - got);
            if (r <= 0)
            {
                close(efd), efd = -1;
                continue;
            }
            got += r;
            if (got >= sizeof(b) - 1) memmove(b, b + got / 2, got - got / 2), got -= got / 2;
        }
    }
    if (efd >= 0) close(efd);
    if (ofd >= 0) close(ofd);
    int st = 0;
    /* its stderr closed is not its end */
    pid_t w;
    while ((w = waitpid(pid, &st, WNOHANG)) == 0 && now_ms() < end)
    {
        dbus_notify("WATCHDOG=1");
        poll(NULL, 0, 100);
    }
    if (w == 0)
    {
        kill(pid, SIGKILL);
        waitpid(pid, &st, 0);
    }
    b[got] = 0;
    while (got && (b[got - 1] == '\n' || b[got - 1] == '\r')) b[--got] = 0;
    char *last = strrchr(b, '\n');
    snprintf(err, n, "%s", last ? last + 1 : b);
    if (WIFEXITED(st) && WEXITSTATUS(st) == 0) return 0;
    if (!err[0]) snprintf(err, n, "%s %s: %s %d", helper, action, WIFEXITED(st) ? "exit" : "signal",
                          WIFEXITED(st) ? WEXITSTATUS(st) : WTERMSIG(st));
    return -1;
}

static int run_helper(const char *helper, const char *action, const char *input, char *err, size_t n)
{
    const char *argv[] = { helper, action, NULL };
    return run_cmd(argv, input, NULL, err, n);
}

int daemon_disk_stop(struct hl *h, const char *why, char *err, size_t n)
{
    int was = h->disk_on;
    hl_disk_end(h);
    int r = run_helper(h->c.disk_helper, "stop", NULL, err, n);
    if (r) hl_log(4, "the installer's USB disk: %s", err);
    if (was) hl_event(h, 5, "agent_disk", "the installer's USB disk for the host taken away (%s)", why);
    return r;
}

int daemon_disk_start(struct hl *h, struct buf *out, char *err, size_t n)
{
    char e2[160];
    if (h->disk_on) daemon_disk_stop(h, "a new one", e2, sizeof(e2));
    if (hl_disk_begin(h, out, err, n)) return -1;
    char line[sizeof(h->pair_str) + 2];
    snprintf(line, sizeof(line), "%s\n", h->pair_str);
    int r = run_helper(h->c.disk_helper, "start", line, e2, sizeof(e2));
    OPENSSL_cleanse(line, sizeof(line));
    if (r)
    {
        /* no disk: nor a pairing nobody got */
        hl_disk_end(h);
        OPENSSL_cleanse(h->token, sizeof(h->token));
        if (out->s) OPENSSL_cleanse(out->s, out->len);
        out->len = 0;
        snprintf(err, n, "USB disk: %s", e2);
        hl_log(4, "the installer's USB disk: %s", e2);
        run_helper(h->c.disk_helper, "stop", NULL, e2, sizeof(e2));
        return -1;
    }
    hl_event(h, 5, "agent_disk", "the installer's USB disk for the host on for %d min", h->c.pair_ttl_ms / 60000);
    return 0;
}

/* ------------------------------------------------------------------ the USB network */
#define USBNET_TIMEOUT_MS 20000
#define USBNET_FRESH_MS   5000

/* the helper's run: a child, its stdout and stderr pipes in the poll set */
static struct
{
    pid_t pid;          /* 0: none */
    int ofd, efd;
    int exited, st;
    int64_t started, exited_at;
    int set, on;        /* a set (else a status) */
    char what[16];
    struct buf out;
    char err[1024];
    size_t elen;
    hl_usbnet_done done;
    void *ctx;
} U = { .ofd = -1, .efd = -1 };
static struct buf s_us; /* the last status answer */
static int64_t s_us_at; /* when (0: none yet) */

static void usbnet_close_fds(void)
{
    if (U.ofd >= 0) close(U.ofd);
    if (U.efd >= 0) close(U.efd);
    U.ofd = U.efd = -1;
}

static int usbnet_spawn(struct hl *h, const char *const argv[], char *err, size_t n)
{
    int so[2], se[2];
    if (pipe2(so, O_CLOEXEC | O_NONBLOCK)) return snprintf(err, n, "pipe: %s", strerror(errno)), -1;
    if (pipe2(se, O_CLOEXEC | O_NONBLOCK))
    {
        close(so[0]), close(so[1]);
        return snprintf(err, n, "pipe: %s", strerror(errno)), -1;
    }
    pid_t pid = fork();
    if (pid < 0)
    {
        close(so[0]), close(so[1]), close(se[0]), close(se[1]);
        return snprintf(err, n, "fork: %s", strerror(errno)), -1;
    }
    if (pid == 0)
    {
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        signal(SIGPIPE, SIG_DFL);
        int nul = open("/dev/null", O_RDONLY);
        if (nul >= 0) dup2(nul, 0);
        dup2(so[1], 1);
        dup2(se[1], 2);
        for (int fd = 3; fd < 1024; fd++) close(fd);
        execv(argv[0], (char *const *)argv);
        fprintf(stderr, "%s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    close(so[1]);
    close(se[1]);
    buf_free(&U.out);
    memset(&U.out, 0, sizeof(U.out));
    U.pid = pid;
    U.ofd = so[0];
    U.efd = se[0];
    U.exited = U.st = 0;
    U.started = now_ms();
    U.elen = 0;
    U.err[0] = 0;
    return 0;
}

static void usbnet_kill(void)
{
    if (!U.pid) return;
    if (!U.exited)
    {
        kill(U.pid, SIGKILL);
        waitpid(U.pid, &U.st, 0);
    }
    usbnet_close_fds();
    U.pid = 0;
}

static void usbnet_status_start(struct hl *h)
{
    char e[160];
    const char *argv[] = { h->c.usbnet_helper, "status", NULL };
    U.set = 0;
    U.done = NULL;
    if (usbnet_spawn(h, argv, e, sizeof(e)))
    {
        buf_free(&s_us);
        memset(&s_us, 0, sizeof(s_us));
        hl_usbnet_answer(NULL, -1, e, &s_us);
        s_us_at = now_ms();
    }
}

void daemon_usbnet_status(struct hl *h, struct buf *out)
{
    if (s_us_at && s_us.s) buf_add(out, s_us.s, s_us.len);
    else buf_printf(out, "{\"pending\":true}");
    if (!U.pid && (!s_us_at || now_ms() - s_us_at >= USBNET_FRESH_MS)) usbnet_status_start(h);
}

int daemon_usbnet_set(struct hl *h, const char *what, int on, hl_usbnet_done done, void *ctx, char *err, size_t n)
{
    err[0] = 0;
    if (hl_usbnet_check(what, err, n)) return -1;
    if (U.pid && U.set) return snprintf(err, n, "busy: another change of the USB network is being applied"), -1;
    usbnet_kill(); /* a status refresh: the change's answer is the new status */
    const char *argv[] = { h->c.usbnet_helper, "set", what, on ? "on" : "off", NULL };
    if (usbnet_spawn(h, argv, err, n)) return -1;
    U.set = 1;
    U.on = on;
    snprintf(U.what, sizeof(U.what), "%s", what);
    U.done = done;
    U.ctx = ctx;
    return 0;
}

static void usbnet_read(void)
{
    char b[1024];
    ssize_t r;
    while (U.ofd >= 0 && (r = read(U.ofd, b, sizeof(b))) != 0)
    {
        if (r < 0)
        {
            if (errno == EINTR) continue;
            if (errno != EAGAIN) close(U.ofd), U.ofd = -1;
            break;
        }
        if (U.out.len + r <= 16384) buf_add(&U.out, b, r);
    }
    if (U.ofd >= 0 && r == 0) close(U.ofd), U.ofd = -1;
    while (U.efd >= 0 && (r = read(U.efd, U.err + U.elen, sizeof(U.err) - 1 - U.elen)) != 0)
    {
        if (r < 0)
        {
            if (errno == EINTR) continue;
            if (errno != EAGAIN) close(U.efd), U.efd = -1;
            break;
        }
        U.elen += r;
        if (U.elen >= sizeof(U.err) - 1)
        {
            memmove(U.err, U.err + U.elen / 2, U.elen - U.elen / 2);
            U.elen -= U.elen / 2;
        }
    }
    if (U.efd >= 0 && r == 0) close(U.efd), U.efd = -1;
}

/* every turn of the loop: what the helper wrote, whether it is done */
static void usbnet_poll(struct hl *h)
{
    if (!U.pid) return;
    int64_t now = now_ms();
    usbnet_read();
    if (!U.exited && waitpid(U.pid, &U.st, WNOHANG) == U.pid) U.exited = 1, U.exited_at = now;
    int timeout = now - U.started >= USBNET_TIMEOUT_MS;
    /* done: exited and its pipes closed, or something it left behind holds them (1 s) */
    if (!timeout && !(U.exited && ((U.ofd < 0 && U.efd < 0) || now - U.exited_at >= 1000))) return;
    char e[300] = "";
    int rc;
    if (timeout && !U.exited)
    {
        usbnet_kill();
        snprintf(e, sizeof(e), "%s %s: timed out after %d s (killed)", h->c.usbnet_helper, U.set ? "set" : "status",
                 USBNET_TIMEOUT_MS / 1000);
        rc = -1;
    }
    else
    {
        usbnet_close_fds();
        U.pid = 0;
        U.err[U.elen] = 0;
        size_t l = U.elen;
        while (l && (U.err[l - 1] == '\n' || U.err[l - 1] == '\r')) U.err[--l] = 0;
        char *last = strrchr(U.err, '\n');
        snprintf(e, sizeof(e), "%s", last ? last + 1 : U.err);
        rc = WIFEXITED(U.st) && WEXITSTATUS(U.st) == 0 ? 0 : -1;
        if (rc && !e[0])
            snprintf(e, sizeof(e), "%s %s: %s %d", h->c.usbnet_helper, U.set ? "set" : "status", WIFEXITED(U.st) ? "exit" : "signal",
                     WIFEXITED(U.st) ? WEXITSTATUS(U.st) : WTERMSIG(U.st));
    }
    struct buf a = { 0 };
    hl_usbnet_answer(U.out.s, rc, e, &a);
    buf_free(&U.out);
    memset(&U.out, 0, sizeof(U.out));
    if (rc == 0 || !U.set)
    {
        /* the new status (a set prints it): the cache */
        buf_free(&s_us);
        s_us = a;
        memset(&a, 0, sizeof(a));
        s_us_at = now;
    }
    if (U.set)
    {
        if (rc) hl_log(4, "USB network: %s %s: %s", U.what, U.on ? "on" : "off", e[0] ? e : "reimu-usbnet failed");
        else if (!strcmp(U.what, "internet"))
            hl_event(h, 5, "usbnet", "USB network: internet for the host %s (through the BMC's port)", U.on ? "on" : "off");
        else hl_event(h, 5, "usbnet", "USB network with the host %s", U.on ? "on" : "off");
        hl_usbnet_done done = U.done;
        U.done = NULL;
        U.set = 0;
        if (done) done(U.ctx, rc == 0, s_us.s ? s_us.s : "{}", e[0] ? e : "reimu-usbnet failed");
        if (rc) s_us_at = 0; /* what it left: asked again */
    }
    buf_free(&a);
}

static int usbnet_pollfds(struct pollfd *p)
{
    int n = 0;
    if (U.ofd >= 0) p[n++] = (struct pollfd){ U.ofd, POLLIN, 0 };
    if (U.efd >= 0) p[n++] = (struct pollfd){ U.efd, POLLIN, 0 };
    return n;
}

/* the daemon stops: a change still running answered */
static void usbnet_stop(void)
{
    if (U.pid && U.set && U.done) U.done(U.ctx, 0, "{}", "reimu-hostlink stopped");
    U.done = NULL;
    usbnet_kill();
}

/* ------------------------------------------------------------------ ctl */
static void ctl_reply(int fd, int ok, const char *err, const char *data)
{
    struct buf b = { 0 };
    buf_printf(&b, "{\"ok\":%s,\"err\":", ok ? "true" : "false");
    buf_json_str(&b, err ? err : "");
    buf_printf(&b, ",\"data\":%s}\n", data && *data ? data : "null");
    if (b.s && write(fd, b.s, b.len) < 0) { /* the client went */ }
    buf_free(&b);
}

/* a change of the USB network done: its ctl connection answered and closed */
static void ctl_usbnet_done(void *ctx, int ok, const char *json, const char *err)
{
    int fd = (int)(intptr_t)ctx;
    ctl_reply(fd, ok, ok ? "" : err, json);
    close(fd);
}

/* 1: the connection kept, answered later (a USB network change) */
static int ctl_command(int fd, char *cmd)
{
    char err[160] = "", a[16] = "";
    double d = 0;
    cmd[strcspn(cmd, "\r\n")] = 0;
    int r = 0;
    struct buf out = { 0 };
#define IS(p) (!strncmp(cmd, p " ", strlen(p) + 1))
    if (!strcmp(cmd, "state")) hl_json(&H, &out);
    else if (IS("fan") && sscanf(cmd + 4, "%15s %lf", a, &d) >= 1) r = hl_set_gpu_fan(&H, a, d, err, sizeof(err));
    else if (IS("turbo") && sscanf(cmd + 6, "%lf", &d) == 1) r = hl_gpu_turbo(&H, (int)d, err, sizeof(err));
    else if (IS("power") && sscanf(cmd + 6, "%15s", a) == 1) r = hl_os_power(&H, a, err, sizeof(err));
    else if (!strcmp(cmd, "edac")) r = hl_edac_load(&H, err, sizeof(err));
    else if (!strcmp(cmd, "inventory")) r = hl_request_inventory(&H, err, sizeof(err));
    else if (!strcmp(cmd, "diag")) r = hl_host_diag(&H, err, sizeof(err));
    else if (!strcmp(cmd, "pair")) r = hl_pair_begin(&H, &out, err, sizeof(err));
    else if (!strcmp(cmd, "unpair")) hl_unpair(&H);
    else if (!strcmp(cmd, "disk")) r = daemon_disk_start(&H, &out, err, sizeof(err));
    else if (!strcmp(cmd, "diskoff")) daemon_disk_stop(&H, "asked", err, sizeof(err)), err[0] = 0;
    else if (!strcmp(cmd, "usbnet")) daemon_usbnet_status(&H, &out);
    else if (IS("usbnet"))
    {
        char w[16], o[8], extra[2];
        if (sscanf(cmd + 7, "%15s %7s %1s", w, o, extra) != 2 || (strcmp(o, "on") && strcmp(o, "off")))
            r = -1, snprintf(err, sizeof(err), "usbnet internet|enabled on|off");
        else if ((r = daemon_usbnet_set(&H, w, !strcmp(o, "on"), ctl_usbnet_done, (void *)(intptr_t)fd, err, sizeof(err))) == 0)
            return 1;
    }
#ifndef HL_DBUS
    else if (IS("fandturbo") && sscanf(cmd + 10, "%lf", &d) == 1) hl_fand_turbo(&H, (int)d);
#endif
#undef IS
    else r = -1, snprintf(err, sizeof(err), "unknown command");
    ctl_reply(fd, r == 0, err, out.s);
    buf_free(&out);
    return 0;
}

static void ctl_accept(void)
{
    int fd = accept4(s_ctl, NULL, NULL, SOCK_CLOEXEC);
    if (fd < 0) return;
    struct timeval tv = { 1, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    char b[256];
    ssize_t n = read(fd, b, sizeof(b) - 1);
    if (n > 0)
    {
        b[n] = 0;
        if (ctl_command(fd, b)) return;
    }
    close(fd);
}

static int ctl_open(void)
{
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof(a.sun_path), "%s/ctl", s_run);
    unlink(a.sun_path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    mode_t um = umask(077);
    int r = fd >= 0 ? bind(fd, (struct sockaddr *)&a, sizeof(a)) : -1;
    umask(um);
    if (r || listen(fd, 4))
    {
        hl_log(4, "%s: %s", a.sun_path, strerror(errno));
        if (fd >= 0) close(fd);
        return -1;
    }
    return fd;
}

static int ctl_client(const char *cmd)
{
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof(a.sun_path), "%s/ctl", s_run);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof(a)))
    {
        fprintf(stderr, "reimu-hostlink is not running (%s: %s)\n", a.sun_path, strerror(errno));
        return 1;
    }
    char line[256];
    int l = snprintf(line, sizeof(line), "%s\n", cmd);
    if (write(fd, line, l) != l) return 1;
    char b[65536];
    ssize_t n;
    int ok = 0;
    while ((n = read(fd, b, sizeof(b))) > 0)
    {
        if (!ok) ok = n >= 10 && !strncmp(b, "{\"ok\":true", 10) ? 1 : -1;
        fwrite(b, 1, n, stdout);
    }
    close(fd);
    return ok == 1 ? 0 : 1;
}

/* ------------------------------------------------------------------ outputs */
static void publish_files(void)
{
    static unsigned sens_gen = ~0u, gen = ~0u;
    static int64_t last_state, metrics_at = -1;
    /* every fresh sample, the same value or not: reimu-fand judges the file
     * by its age (a steady 96 °C must keep driving the board fans); when the
     * samples stop, the value expires (NAN after LOST_SEC: the file goes) */
    if (H.sens_gen != sens_gen || H.metrics_at != metrics_at)
    {
        sens_gen = H.sens_gen;
        metrics_at = H.metrics_at;
        double t = hl_gpu_temp(&H);
        char s[32];
        snprintf(s, sizeof(s), "%ld\n", (long)lround(t * 1000));
        put_run("gpu-temp", isnan(t) ? NULL : s);
    }
#ifndef HL_DBUS
    if (H.gen != gen && H.now - last_state >= 200)
    {
        gen = H.gen;
        last_state = H.now;
        struct buf b = { 0 };
        hl_json(&H, &b);
        put_run("state.json", b.s);
        buf_free(&b);
    }
#else
    (void)gen;
    (void)last_state;
#endif
}

/* where the host's H.264 is, for reimu-kvm (hl_kvm_hint): a datagram to its
 * port on loopback every 2 s (reimu-kvm keeps it 15 s) */
static void kvm_hint_send(void)
{
    static int fd = -1;
    if (!H.c.kvm_port) return;
    if (fd < 0) fd = socket(AF_INET6, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return;
    char m[160];
    int n = hl_kvm_hint(&H, m, sizeof(m));
    struct sockaddr_in6 a = { .sin6_family = AF_INET6, .sin6_port = htons(H.c.kvm_port), .sin6_addr = IN6ADDR_LOOPBACK_INIT };
    if (n > 0 && (size_t)n < sizeof(m)) (void)!sendto(fd, m, n, 0, (struct sockaddr *)&a, sizeof(a));
}

/* ------------------------------------------------------------------ main */
static int run(void)
{
    const char *d = getenv("REIMU_HOSTLINK_DIR");
    char dir[160];
    snprintf(dir, sizeof(dir), "%s", d && *d ? d : "/var/lib/reimu-hostlink");
    mkdir(dir, 0700);
    mkdir(s_run, 0755);
    struct hl_config c;
    load_config(&c, s_conf, 0);
    hl_init(&H, &c, dir);
    H.on_diag = on_diag;
    char cert[200], err[200];
    snprintf(cert, sizeof(cert), "%s/cert.pem", dir);
    s_net = net_new(&H, cert, err, sizeof(err));
    if (!s_net)
    {
        hl_log(2, "%s", err);
        return 1;
    }
    sigset_t ss;
    sigemptyset(&ss);
    sigaddset(&ss, SIGTERM);
    sigaddset(&ss, SIGINT);
    sigaddset(&ss, SIGHUP);
    sigprocmask(SIG_BLOCK, &ss, NULL);
    signal(SIGPIPE, SIG_IGN);
    int sfd = signalfd(-1, &ss, SFD_NONBLOCK | SFD_CLOEXEC);
    s_ctl = ctl_open();
    if (dbus_init(&H))
    {
        hl_log(2, "D-Bus setup failed");
        return 1;
    }
    hl_log(5, "started: port %d%s%s, %s, certificate %.16s…", H.c.port, H.c.iface[0] ? " on " : "", H.c.iface,
           H.paired ? "paired" : "not paired", H.fingerprint);
    /* a disk a crash left behind (its pairing string is gone with the old process) */
    if (access(H.c.disk_helper, X_OK) == 0) daemon_disk_stop(&H, "start", err, sizeof(err));
    /* the USB network's status ready for the page's first look */
    usbnet_status_start(&H);
    dbus_notify("READY=1");
    int64_t last_tick = 0, last_wd = 0;
    for (;;)
    {
        struct pollfd p[24];
        int n = net_pollfds(s_net, p, 16);
        int nn = n;
        p[nn++] = (struct pollfd){ sfd, POLLIN, 0 };
        if (s_ctl >= 0) p[nn++] = (struct pollfd){ s_ctl, POLLIN, 0 };
        int bi = nn;
        nn += dbus_pollfd(&p[nn]);
        int ui = nn;
        nn += usbnet_pollfds(&p[nn]);
        int64_t now = now_ms();
        int timeout = (int)(1000 - (now - last_tick));
        if (timeout < 0) timeout = 0;
        int bt = dbus_timeout_ms();
        if (bt >= 0 && bt < timeout) timeout = bt;
        if (poll(p, nn, timeout) < 0 && errno != EINTR) break;
        now = now_ms();
        if (p[n].revents)
        {
            struct signalfd_siginfo si;
            if (read(sfd, &si, sizeof(si)) == sizeof(si))
            {
                if (si.ssi_signo == SIGHUP)
                {
                    struct hl_config nc;
                    load_config(&nc, s_conf, 0);
                    int port = H.c.port;
                    char iface[sizeof(H.c.iface)];
                    memcpy(iface, H.c.iface, sizeof(iface));
                    H.c = nc;
                    H.c.port = port; /* the sockets stay (a restart for those) */
                    memcpy(H.c.iface, iface, sizeof(iface));
                    hl_log(5, "SIGHUP: %s re-read", s_conf);
                }
                else break;
            }
        }
        if (s_ctl >= 0 && p[n + 1].revents) ctl_accept();
        net_handle(s_net, p, n, now);
        if (bi < ui) dbus_process();
        usbnet_poll(&H);
        if (now - last_tick >= 1000)
        {
            last_tick = now;
            hl_tick(&H, now);
            dbus_tick(&H, now);
            if (hl_disk_due(&H)) daemon_disk_stop(&H, now >= H.disk_until ? "time up" : "the agent installed", err, sizeof(err));
            static int64_t last_hint;
            if (now - last_hint >= 2000)
            {
                last_hint = now;
                kvm_hint_send();
            }
        }
        H.now = now;
        net_handle(s_net, p, 0, now); /* what the D-Bus calls and the tick queued */
        dbus_publish(&H);
        publish_files();
        if (now - last_wd >= 5000)
        {
            last_wd = now;
            dbus_notify("WATCHDOG=1");
        }
    }
    usbnet_stop();
    dbus_publish(&H); /* its answer out */
    if (H.disk_on) daemon_disk_stop(&H, "reimu-hostlink stopped", err, sizeof(err));
    hl_log(5, "stopped");
    put_run("gpu-temp", NULL);
    net_free(s_net);
    dbus_close();
    return 0;
}

int main(int argc, char **argv)
{
    const char *cf = getenv("REIMU_HOSTLINK_CONF");
    if (cf && *cf) s_conf = cf;
    const char *rd = getenv("REIMU_HOSTLINK_RUN");
    if (rd && *rd) snprintf(s_run, sizeof(s_run), "%s", rd);
    if (argc >= 2 && argc <= 3 && !strcmp(argv[1], "--check-config"))
    {
        struct hl_config c;
        int n = load_config(&c, argc == 3 ? argv[2] : s_conf, 1);
        printf("PORT=%d IFACE=%s ALLOW_IPV4=%s PING_SEC=%d LOST_SEC=%d FOLLOW_FAND_TURBO=%s LOGGING=%s\n", c.port, c.iface,
               c.allow_ipv4 ? "yes" : "no", c.ping_ms / 1000, c.lost_ms / 1000, c.follow_fand_turbo ? "yes" : "no",
               c.logging ? "yes" : "no");
        printf("GPU warn/crit °C: edge %g/%g junction %g/%g memory %g/%g\n", c.warn[0], c.crit[0], c.warn[1], c.crit[1], c.warn[2],
               c.crit[2]);
        printf("USBNET_HELPER=%s DISK_HELPER=%s\n", c.usbnet_helper, c.disk_helper);
        return n ? 1 : 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--ctl")) return ctl_client(argv[2]);
    if (argc != 1)
    {
        fprintf(stderr, "usage: %s [--check-config [FILE] | --ctl COMMAND]\n", argv[0]);
        return 2;
    }
    return run();
}
