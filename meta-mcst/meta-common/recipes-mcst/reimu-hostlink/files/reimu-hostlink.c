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
 *   sensors   /xyz/openbmc_project/sensors/{temperature,fan_tach,fan_pwm,power}/Host_*
 *             (Sensor.Value, Availability, OperationalStatus, thresholds,
 *             the chassis association: Redfish, IPMI SDR)
 *
 * Files: /etc/reimu-hostlink.conf (KEY=VALUE, see the file), the state in
 * /var/lib/reimu-hostlink (key 0600, cert.pem 0600, policy, edac), and in
 * /run/reimu-hostlink: gpu-temp (the GPU's junction temperature in m°C, like
 * a hwmon file, for reimu-fand; absent without a fresh value) and ctl (a
 * root-only socket: reimu-hostlink --ctl CMD).
 *
 *   reimu-hostlink                 the daemon (systemd: Type=notify, watchdog)
 *   reimu-hostlink --check-config [FILE]
 *   reimu-hostlink --ctl 'state' | 'fan manual 40' | 'turbo 5' | 'power reboot'
 *                  | 'edac' | 'inventory' | 'pair' | 'unpair' | 'disk' | 'diskoff'
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

/* ------------------------------------------------------------------ the installer's USB disk */
/* DISK_HELPER ACTION, INPUT on its stdin, at most 15 s; 0 = it exited 0, else
 * its last line on stderr in err */
static int run_helper(const char *helper, const char *action, const char *input, char *err, size_t n)
{
    int in[2], er[2];
    err[0] = 0;
    if (pipe2(in, O_CLOEXEC)) return snprintf(err, n, "pipe: %s", strerror(errno)), -1;
    if (pipe2(er, O_CLOEXEC))
    {
        close(in[0]), close(in[1]);
        return snprintf(err, n, "pipe: %s", strerror(errno)), -1;
    }
    pid_t pid = fork();
    if (pid < 0)
    {
        close(in[0]), close(in[1]), close(er[0]), close(er[1]);
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
        int nul = open("/dev/null", O_WRONLY);
        if (nul >= 0) dup2(nul, 1);
        for (int fd = 3; fd < 1024; fd++) close(fd);
        execl(helper, helper, action, (char *)NULL);
        fprintf(stderr, "%s: %s\n", helper, strerror(errno));
        _exit(127);
    }
    close(in[0]);
    close(er[1]);
    if (input && *input && write(in[1], input, strlen(input)) < 0) { /* it went: its status says why */ }
    close(in[1]);
    char b[1024];
    size_t got = 0;
    int64_t end = now_ms() + 15000;
    for (;;)
    {
        int64_t left = end - now_ms();
        if (left <= 0) break;
        struct pollfd p = { er[0], POLLIN, 0 };
        if (poll(&p, 1, (int)left) <= 0) break;
        ssize_t r = read(er[0], b + got, sizeof(b) - 1 - got);
        if (r <= 0) break;
        got += r;
        if (got >= sizeof(b) - 1) memmove(b, b + got / 2, got - got / 2), got -= got / 2;
    }
    close(er[0]);
    int st = 0;
    if (waitpid(pid, &st, WNOHANG) == 0)
    {
        if (now_ms() >= end) kill(pid, SIGKILL);
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

static void ctl_command(int fd, char *cmd)
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
    else if (!strcmp(cmd, "pair")) r = hl_pair_begin(&H, &out, err, sizeof(err));
    else if (!strcmp(cmd, "unpair")) hl_unpair(&H);
    else if (!strcmp(cmd, "disk")) r = daemon_disk_start(&H, &out, err, sizeof(err));
    else if (!strcmp(cmd, "diskoff")) daemon_disk_stop(&H, "asked", err, sizeof(err)), err[0] = 0;
#ifndef HL_DBUS
    else if (IS("fandturbo") && sscanf(cmd + 10, "%lf", &d) == 1) hl_fand_turbo(&H, (int)d);
#endif
#undef IS
    else r = -1, snprintf(err, sizeof(err), "unknown command");
    ctl_reply(fd, r == 0, err, out.s);
    buf_free(&out);
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
        ctl_command(fd, b);
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
                    char iface[16];
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
        if (bi < nn) dbus_process();
        if (now - last_tick >= 1000)
        {
            last_tick = now;
            hl_tick(&H, now);
            dbus_tick(&H, now);
            if (hl_disk_due(&H)) daemon_disk_stop(&H, now >= H.disk_until ? "time up" : "the agent installed", err, sizeof(err));
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
