/*
 * reimu-fand: fan control of REIMU-5564 (1Э8СВ-uATX), the only one: the
 * host's pwmd doesn't drive these fans. Control logic in fand.c.
 *
 *   service   xyz.openbmc_project.Reimu.Fans
 *   object    /xyz/openbmc_project/reimu/fans
 *   interface xyz.openbmc_project.Reimu.Fans
 *     property Json (s)        the whole state, see fand_json()
 *     property Alarm (b), AlarmText (s), Host (s)
 *     method   SetMode(s zone, s mode)          auto | manual | full
 *     method   SetDuty(s zone, d duty)          manual mode at that duty
 *     method   Confirm(s zone)                  manual mode for another
 *                                               MANUAL_TIMEOUT_SEC
 *     method   SetCurve(s zone, s input, s points) -> s   checked, applied,
 *                                               saved in the config file
 *     method   ClearAlarms()
 *     method   Reload() -> s                    re-reads the config file,
 *                                               returns its errors (JSON)
 *   objects   /xyz/openbmc_project/reimu/fans/cpu, .../board
 *   interface xyz.openbmc_project.Reimu.Fans.Zone
 *     property Mode (s, writable), ManualDuty (d, writable), Duty (d),
 *              Demand (d), Failsafe (b), Reason (s), Rpm (d), Curves (s)
 *
 * Host power: CurrentHostState of xyz.openbmc_project.State.Host (set by
 * reimupwr from GPIO_POWER_IN; the GPIO itself is not requested here: a
 * second user of the line makes reimupwr exit). When that says Off or can't
 * be read, the platform devices are probed every 10 s (HOST_PROBE): if they
 * answer, the platform is on whatever D-Bus says.
 *
 *   reimu-fand                 the daemon (systemd: Type=notify, watchdog)
 *   reimu-fand --failsafe      every fan to 100 % and exit (ExecStopPost)
 *   reimu-fand --check-config [FILE]
 *   reimu-fand --status        the state (JSON) of the running daemon
 *   reimu-fand --observe       reads and computes, writes nothing (no fan,
 *                              no LED), prints what it would do every 5 s;
 *                              alongside anything (no D-Bus name)
 *   reimu-fand --no-dbus       the daemon without D-Bus (tests): host power
 *                              from the file $REIMU_FAND_HOST (on/off), or
 *                              by probing
 * REIMU_FAND_CONF: config file (default /etc/reimu-fand.conf),
 * REIMU_FAND_RUN: state directory (default /run/reimu-fand),
 * REIMU_FAND_FAKE: fake hardware, see fand.h.
 *
 * Copyright (c) 2026 MCST REIMU contributors. Licensed under GPLv2.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <systemd/sd-bus.h>
#include <systemd/sd-daemon.h>
#include <systemd/sd-event.h>

#include "fand.h"

#define SERVICE   "xyz.openbmc_project.Reimu.Fans"
#define PATH      "/xyz/openbmc_project/reimu/fans"
#define IFACE     "xyz.openbmc_project.Reimu.Fans"
#define ZIFACE    "xyz.openbmc_project.Reimu.Fans.Zone"
#define HOST_SVC  "xyz.openbmc_project.State.Host"
#define HOST_PATH "/xyz/openbmc_project/state/host0"
#define PROBE_MS  10000
#define QUERY_MS  5000
#define PENDING_MS 5000

static struct fand F;
static const char *s_conf = "/etc/reimu-fand.conf";
static char s_runfile[256];
static sd_bus *s_bus;
static sd_event *s_ev;
static int s_nodbus;
static int s_observe;

/* host power from D-Bus: -1 unknown, 0 off, 1 on */
static int s_dbus_host = -1;
static int s_query_errors;
static sd_bus_slot *s_query;
static int64_t s_last_query, s_start;
/* probing: last time, last result */
static int64_t s_last_probe = -PROBE_MS;
static int s_probe_on;
static int s_override_logged;
static int64_t s_saved_on_since = -2;
static unsigned s_last_gen = ~0u;
static int64_t s_next_tick;

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ------------------------------------------------------------------ config */
static void load_config(int initial)
{
    struct config c;
    char err[FAND_MAX_ERR][128];
    cfg_defaults(&c);
    int n = cfg_load(&c, s_conf, err, FAND_MAX_ERR);
    for (int i = 0; i < n && i < FAND_MAX_ERR; i++) fand_log(4, "%s: %s", s_conf, err[i]);
    if (initial) fand_init(&F, &c, now_ms());
    else fand_reconfig(&F, &c);
    F.nerr = n < FAND_MAX_ERR ? n : FAND_MAX_ERR;
    memcpy(F.err, err, sizeof(err[0]) * F.nerr);
}

/* /run/reimu-fand/host-on: since when (CLOCK_MONOTONIC ms) the host has been
 * on, so that a restarted daemon doesn't wait SETTLE_SEC again */
static void save_on_since(void)
{
    int64_t v = F.phase == PH_SETTLE || F.phase == PH_RUN ? F.on_since : -1;
    if (v == s_saved_on_since) return;
    s_saved_on_since = v;
    if (v < 0)
    {
        unlink(s_runfile);
        return;
    }
    char tmp[300], s[32];
    snprintf(tmp, sizeof(tmp), "%s.new", s_runfile);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return;
    int len = snprintf(s, sizeof(s), "%lld\n", (long long)v);
    int ok = write(fd, s, len) == len;
    close(fd);
    if (ok) rename(tmp, s_runfile);
    else unlink(tmp);
}

static void load_on_since(void)
{
    char s[32];
    int fd = open(s_runfile, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    ssize_t n = read(fd, s, sizeof(s) - 1);
    close(fd);
    if (n <= 0) return;
    s[n] = 0;
    char *e;
    long long v = strtoll(s, &e, 10);
    if (e != s && v >= 0) F.on_since_hint = v;
}

/* ------------------------------------------------------------------ host state */
static void set_dbus_host(const char *st)
{
    int on = !(strlen(st) >= 4 && !strcmp(st + strlen(st) - 4, ".Off"));
    if (on != s_dbus_host) fand_log(6, "host state %s", st);
    s_dbus_host = on;
    s_query_errors = 0;
}

static int on_props(sd_bus_message *m, void *ud, sd_bus_error *err)
{
    const char *iface;
    if (sd_bus_message_read(m, "s", &iface) < 0 || strcmp(iface, HOST_SVC)) return 0;
    if (sd_bus_message_enter_container(m, 'a', "{sv}") < 0) return 0;
    while (sd_bus_message_enter_container(m, 'e', "sv") > 0)
    {
        const char *name, *val;
        if (sd_bus_message_read(m, "s", &name) < 0) return 0;
        if (!strcmp(name, "CurrentHostState") && sd_bus_message_read(m, "v", "s", &val) >= 0) set_dbus_host(val);
        else sd_bus_message_skip(m, "v");
        sd_bus_message_exit_container(m);
    }
    return 0;
}

static int on_get(sd_bus_message *m, void *ud, sd_bus_error *err)
{
    const char *val;
    s_query = sd_bus_slot_unref(s_query);
    if (sd_bus_message_is_method_error(m, NULL) || sd_bus_message_read(m, "v", "s", &val) < 0)
    {
        if (++s_query_errors == 3)
        {
            fand_log(4, "host state unreadable over D-Bus (%s): judged by the platform devices",
                     sd_bus_message_get_error(m) ? sd_bus_message_get_error(m)->message : "bad reply");
            s_dbus_host = -1;
        }
        return 0;
    }
    set_dbus_host(val);
    return 0;
}

static void query_host(int64_t now)
{
    if (!s_bus || s_query || now - s_last_query < QUERY_MS) return;
    s_last_query = now;
    int r = sd_bus_call_method_async(s_bus, &s_query, HOST_SVC, HOST_PATH, "org.freedesktop.DBus.Properties", "Get", on_get,
                                     NULL, "ss", HOST_SVC, "CurrentHostState");
    if (r < 0 && ++s_query_errors >= 3) s_dbus_host = -1;
}

static int host_from_file(void)
{
    const char *p = getenv("REIMU_FAND_HOST");
    if (!p) return -1;
    char s[16] = "";
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = read(fd, s, sizeof(s) - 1);
    close(fd);
    if (n <= 0) return -1;
    s[n] = 0;
    return !strncmp(s, "on", 2) ? 1 : !strncmp(s, "off", 3) ? 0 : -1;
}

static int decide_host(int64_t now)
{
    int st = s_nodbus ? host_from_file() : s_dbus_host;
    if (st == 1)
    {
        s_override_logged = 0;
        return HOST_ON;
    }
    /* the first answer is on its way */
    if (st < 0 && !s_nodbus && now - s_start < PENDING_MS && !s_query_errors) return HOST_PENDING;
    if (!F.c.host_probe) return st == 0 ? HOST_OFF : HOST_UNKNOWN;
    if (now - s_last_probe >= PROBE_MS)
    {
        s_last_probe = now;
        s_probe_on = fand_probe(&F.c);
    }
    if (!s_probe_on)
    {
        s_override_logged = 0;
        return HOST_OFF;
    }
    if (!s_override_logged)
        fand_log(4, "host state %s, but the platform devices answer: taken as on", st == 0 ? "Off" : "unknown");
    s_override_logged = 1;
    return HOST_UNKNOWN;
}

/* ------------------------------------------------------------------ D-Bus */
static int zone_of_path(const char *path)
{
    const char *p = strrchr(path, '/');
    return p ? zone_by_name(p + 1) : -1;
}

static int prop_root(sd_bus *bus, const char *path, const char *iface, const char *prop, sd_bus_message *reply, void *ud,
                     sd_bus_error *err)
{
    if (!strcmp(prop, "Json"))
    {
        struct buf b = { 0 };
        fand_json(&F, &b);
        int r = sd_bus_message_append(reply, "s", b.s && !b.oom ? b.s : "{}");
        free(b.s);
        return r;
    }
    if (!strcmp(prop, "Alarm")) return sd_bus_message_append(reply, "b", fand_alarm(&F));
    if (!strcmp(prop, "AlarmText"))
    {
        char t[512];
        fand_alarm_text(&F, t, sizeof(t));
        return sd_bus_message_append(reply, "s", t);
    }
    return sd_bus_message_append(reply, "s", fand_host_name(&F));
}

static int prop_zone(sd_bus *bus, const char *path, const char *iface, const char *prop, sd_bus_message *reply, void *ud,
                     sd_bus_error *err)
{
    int zi = zone_of_path(path);
    if (zi < 0) return -ENOENT;
    const struct zone *z = &F.z[zi];
    int run = F.phase == PH_RUN;
    if (!strcmp(prop, "Mode")) return sd_bus_message_append(reply, "s", mode_name[z->mode]);
    if (!strcmp(prop, "ManualDuty")) return sd_bus_message_append(reply, "d", z->mode == MODE_MANUAL ? z->manual_duty : NAN);
    if (!strcmp(prop, "Duty")) return sd_bus_message_append(reply, "d", run ? z->duty : NAN);
    if (!strcmp(prop, "Demand")) return sd_bus_message_append(reply, "d", run ? z->demand : NAN);
    if (!strcmp(prop, "Failsafe")) return sd_bus_message_append(reply, "b", z->failsafe);
    if (!strcmp(prop, "Reason")) return sd_bus_message_append(reply, "s", run ? z->reason : "");
    if (!strcmp(prop, "Rpm")) return sd_bus_message_append(reply, "d", run && z->out[0].has_tach ? z->out[0].rpm : NAN);
    /* Curves: "pcs=45:30 ...;vrm=..." */
    char s[512], one[160];
    size_t len = 0;
    s[0] = 0;
    for (int g = 0; g < NGROUPS; g++)
        if (curve_key(zi, g))
        {
            if (F.c.use[zi][g]) curve_format(&F.c.curve[zi][g], one, sizeof(one));
            else snprintf(one, sizeof(one), "none");
            len += snprintf(s + len, sizeof(s) - len, "%s%s=%s", len ? ";" : "", group_id[g], one);
            if (len >= sizeof(s)) break;
        }
    return sd_bus_message_append(reply, "s", s);
}

static int set_zone(sd_bus *bus, const char *path, const char *iface, const char *prop, sd_bus_message *value, void *ud,
                    sd_bus_error *err)
{
    int zi = zone_of_path(path);
    char e[160];
    if (!strcmp(prop, "Mode"))
    {
        const char *m;
        int r = sd_bus_message_read(value, "s", &m);
        if (r < 0) return r;
        if (fand_set_mode(&F, zi, mode_by_name(m), e, sizeof(e)))
            return sd_bus_error_set(err, SD_BUS_ERROR_INVALID_ARGS, e);
        return 0;
    }
    double d;
    int r = sd_bus_message_read(value, "d", &d);
    if (r < 0) return r;
    if (fand_set_duty(&F, zi, d, e, sizeof(e))) return sd_bus_error_set(err, SD_BUS_ERROR_INVALID_ARGS, e);
    return 0;
}

static int m_set_mode(sd_bus_message *m, void *ud, sd_bus_error *err)
{
    const char *zone, *mode;
    char e[160];
    int r = sd_bus_message_read(m, "ss", &zone, &mode);
    if (r < 0) return r;
    if (fand_set_mode(&F, zone_by_name(zone), mode_by_name(mode), e, sizeof(e)))
        return sd_bus_error_set(err, SD_BUS_ERROR_INVALID_ARGS, e);
    return sd_bus_reply_method_return(m, "");
}

static int m_set_duty(sd_bus_message *m, void *ud, sd_bus_error *err)
{
    const char *zone;
    double d;
    char e[160];
    int r = sd_bus_message_read(m, "sd", &zone, &d);
    if (r < 0) return r;
    if (fand_set_duty(&F, zone_by_name(zone), d, e, sizeof(e))) return sd_bus_error_set(err, SD_BUS_ERROR_INVALID_ARGS, e);
    return sd_bus_reply_method_return(m, "");
}

static int m_confirm(sd_bus_message *m, void *ud, sd_bus_error *err)
{
    const char *zone;
    char e[160];
    int r = sd_bus_message_read(m, "s", &zone);
    if (r < 0) return r;
    if (fand_confirm(&F, zone_by_name(zone), e, sizeof(e))) return sd_bus_error_set(err, SD_BUS_ERROR_INVALID_ARGS, e);
    return sd_bus_reply_method_return(m, "");
}

static int m_set_curve(sd_bus_message *m, void *ud, sd_bus_error *err)
{
    const char *zone, *input, *pts;
    char e[160], s[160];
    int r = sd_bus_message_read(m, "sss", &zone, &input, &pts);
    if (r < 0) return r;
    int zi = zone_by_name(zone), g = group_by_name(input);
    if (fand_set_curve(&F, zi, g, pts, e, sizeof(e))) return sd_bus_error_set(err, SD_BUS_ERROR_INVALID_ARGS, e);
    curve_format(&F.c.curve[zi][g], s, sizeof(s));
    fand_log(5, "zone %s, input %s: curve %s", zone, input, s);
    if (cfg_save_key(s_conf, curve_key(zi, g), s))
    {
        snprintf(e, sizeof(e), "applied, but not saved in %s: %s", s_conf, strerror(errno));
        return sd_bus_error_set(err, SD_BUS_ERROR_IO_ERROR, e);
    }
    return sd_bus_reply_method_return(m, "s", s);
}

static int m_clear(sd_bus_message *m, void *ud, sd_bus_error *err)
{
    fand_clear_alarms(&F);
    return sd_bus_reply_method_return(m, "");
}

static int m_reload(sd_bus_message *m, void *ud, sd_bus_error *err)
{
    load_config(0);
    struct buf b = { 0 };
    buf_printf(&b, "[");
    for (int i = 0; i < F.nerr; i++)
    {
        buf_printf(&b, "%s\"", i ? "," : "");
        for (const char *p = F.err[i]; *p; p++) buf_printf(&b, *p == '"' || *p == '\\' ? "\\%c" : "%c", *p);
        buf_printf(&b, "\"");
    }
    buf_printf(&b, "]");
    int r = sd_bus_reply_method_return(m, "s", b.s && !b.oom ? b.s : "[]");
    free(b.s);
    return r;
}

static const sd_bus_vtable root_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("Json", "s", prop_root, 0, SD_BUS_VTABLE_PROPERTY_EMITS_INVALIDATION),
    SD_BUS_PROPERTY("Alarm", "b", prop_root, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("AlarmText", "s", prop_root, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("Host", "s", prop_root, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_METHOD("SetMode", "ss", "", m_set_mode, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("SetDuty", "sd", "", m_set_duty, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Confirm", "s", "", m_confirm, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("SetCurve", "sss", "s", m_set_curve, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("ClearAlarms", "", "", m_clear, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Reload", "", "s", m_reload, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END
};

static const sd_bus_vtable zone_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_WRITABLE_PROPERTY("Mode", "s", prop_zone, set_zone, 0,
                             SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE | SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_WRITABLE_PROPERTY("ManualDuty", "d", prop_zone, set_zone, 0,
                             SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE | SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_PROPERTY("Duty", "d", prop_zone, 0, 0),
    SD_BUS_PROPERTY("Demand", "d", prop_zone, 0, 0),
    SD_BUS_PROPERTY("Failsafe", "b", prop_zone, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("Reason", "s", prop_zone, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("Rpm", "d", prop_zone, 0, 0),
    SD_BUS_PROPERTY("Curves", "s", prop_zone, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_VTABLE_END
};

static void emit_changes(void)
{
    if (!s_bus || F.gen == s_last_gen) return;
    s_last_gen = F.gen;
    sd_bus_emit_properties_changed(s_bus, PATH, IFACE, "Json", "Alarm", "AlarmText", "Host", NULL);
    for (int zi = 0; zi < NZONES; zi++)
    {
        char p[96];
        snprintf(p, sizeof(p), "%s/%s", PATH, zone_id[zi]);
        sd_bus_emit_properties_changed(s_bus, p, ZIFACE, "Mode", "ManualDuty", "Failsafe", "Reason", "Curves", NULL);
    }
}

/* ------------------------------------------------------------------ loop */
static int on_tick(sd_event_source *s, uint64_t usec, void *ud)
{
    int64_t now = now_ms();
    query_host(now);
    fand_step(&F, now, decide_host(now));
    save_on_since();
    emit_changes();
    static int64_t last_status;
    static unsigned status_gen = ~0u;
    if (F.gen != status_gen || now - last_status >= 10000)
    {
        status_gen = F.gen;
        last_status = now;
        if (F.phase == PH_RUN)
            sd_notifyf(0, "STATUS=host %s, cpu %.0f%% (%s), board %.0f%% (%s)%s", fand_host_name(&F), F.z[Z_CPU].duty,
                       mode_name[F.z[Z_CPU].mode], F.z[Z_BOARD].duty, mode_name[F.z[Z_BOARD].mode],
                       fand_alarm(&F) ? ", ALARM" : "");
        else sd_notifyf(0, "STATUS=host %s", fand_host_name(&F));
    }
    if (s_observe && F.ticks % (5000 / F.c.poll_ms ? 5000 / F.c.poll_ms : 1) == 0)
    {
        if (F.phase != PH_RUN) printf("host %s\n", fand_host_name(&F));
        else
        {
            const struct output *o = &F.z[Z_CPU].out[0];
            printf("would set: cpu %.1f%% board %.1f%% | pcs %.1f vrm %.1f iohub %.1f | cpu fan %.0f rpm, now raw %d/%d | %s\n",
                   F.z[Z_CPU].duty, F.z[Z_BOARD].duty, F.g[G_PCS].max, F.g[G_VRM].max, F.g[G_IOHUB].max, o->rpm, o->raw,
                   F.z[Z_BOARD].out[1].raw, fand_alarm(&F) ? "ALARM" : "ok");
            char t[512];
            fand_alarm_text(&F, t, sizeof(t));
            if (*t) printf("  %s\n", t);
        }
        fflush(stdout);
    }
    /* the watchdog sees the control loop, not just the event loop */
    sd_notify(0, "WATCHDOG=1");
    s_next_tick += F.c.poll_ms;
    if (s_next_tick < now) s_next_tick = now + F.c.poll_ms;
    sd_event_source_set_time(s, (uint64_t)s_next_tick * 1000);
    sd_event_source_set_enabled(s, SD_EVENT_ONESHOT);
    return 0;
}

static int on_signal(sd_event_source *s, const struct signalfd_siginfo *si, void *ud)
{
    if (si->ssi_signo == SIGHUP)
    {
        fand_log(5, "SIGHUP: config re-read");
        load_config(0);
        return 0;
    }
    /* stopping: nobody controls the fans after this */
    if (s_observe) fand_log(5, "observer stopped");
    else
    {
        if (F.phase != PH_OFF) fand_failsafe(&F.c);
        fand_log(5, "stopped: fans 100%%");
    }
    sd_event_exit(s_ev, 0);
    return 0;
}

static int run_daemon(void)
{
    const char *rd = getenv("REIMU_FAND_RUN");
    rd = rd ? rd : "/run/reimu-fand";
    mkdir(rd, 0755);
    snprintf(s_runfile, sizeof(s_runfile), "%s/host-on", rd);
    load_config(1);
    load_on_since();
    s_start = now_ms();

    sigset_t ss;
    sigemptyset(&ss);
    sigaddset(&ss, SIGTERM);
    sigaddset(&ss, SIGINT);
    sigaddset(&ss, SIGHUP);
    sigprocmask(SIG_BLOCK, &ss, NULL);

    int r = sd_event_default(&s_ev);
    if (r < 0)
    {
        fand_log(2, "sd_event: %s", strerror(-r));
        return 1;
    }
    sd_event_add_signal(s_ev, NULL, SIGTERM, on_signal, NULL);
    sd_event_add_signal(s_ev, NULL, SIGINT, on_signal, NULL);
    sd_event_add_signal(s_ev, NULL, SIGHUP, on_signal, NULL);

    if (!s_nodbus)
    {
        r = sd_bus_default_system(&s_bus);
        if (r < 0 && s_observe)
        {
            /* no system bus: host power by probing (or $REIMU_FAND_HOST) */
            fand_log(4, "no D-Bus (%s): host power by probing", strerror(-r));
            s_nodbus = 1;
            s_bus = NULL;
            r = 0;
            goto bus_done;
        }
        if (r >= 0 && !s_observe) r = sd_bus_add_object_vtable(s_bus, NULL, PATH, IFACE, root_vtable, NULL);
        for (int zi = 0; r >= 0 && !s_observe && zi < NZONES; zi++)
        {
            char p[96];
            snprintf(p, sizeof(p), "%s/%s", PATH, zone_id[zi]);
            r = sd_bus_add_object_vtable(s_bus, NULL, strdup(p), ZIFACE, zone_vtable, NULL);
        }
        if (r >= 0 && !s_observe) r = sd_bus_add_object_manager(s_bus, NULL, PATH);
        if (r >= 0)
            r = sd_bus_match_signal_async(s_bus, NULL, NULL, HOST_PATH, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                                          on_props, NULL, NULL);
        if (r >= 0) r = sd_bus_attach_event(s_bus, s_ev, SD_EVENT_PRIORITY_NORMAL);
        if (r >= 0 && !s_observe) r = sd_bus_request_name(s_bus, SERVICE, 0);
        if (r < 0)
        {
            /* systemd starts it again; ExecStopPost puts the fans at 100 % */
            fand_log(2, "D-Bus setup failed: %s", strerror(-r));
            return 1;
        }
    }
bus_done:

    s_next_tick = now_ms();
    sd_event_source *tick;
    r = sd_event_add_time(s_ev, &tick, CLOCK_MONOTONIC, (uint64_t)s_next_tick * 1000, 10000, on_tick, NULL);
    if (r < 0)
    {
        fand_log(2, "timer: %s", strerror(-r));
        return 1;
    }
    char curves[4][160];
    curve_format(&F.c.curve[Z_CPU][G_PCS], curves[0], sizeof(curves[0]));
    curve_format(&F.c.curve[Z_BOARD][G_IOHUB], curves[1], sizeof(curves[1]));
    fand_log(5, "started: floor %g%%, CPU/PCS %s, board/IOHUB %s, %d config error(s)", F.c.floor, curves[0], curves[1], F.nerr);
    sd_notify(0, "READY=1");
    r = sd_event_loop(s_ev);
    sd_bus_flush_close_unref(s_bus);
    sd_event_unref(s_ev);
    return r < 0 ? 1 : 0;
}

static int print_status(void)
{
    sd_bus *bus = NULL;
    sd_bus_error e = SD_BUS_ERROR_NULL;
    char *s = NULL;
    int r = sd_bus_default_system(&bus);
    if (r >= 0) r = sd_bus_get_property_string(bus, SERVICE, PATH, IFACE, "Json", &e, &s);
    if (r < 0) fprintf(stderr, "reimu-fand is not answering: %s\n", e.message ? e.message : strerror(-r));
    else puts(s);
    free(s);
    sd_bus_error_free(&e);
    sd_bus_unref(bus);
    return r < 0;
}

static int check_config(const char *file)
{
    struct config c;
    char err[FAND_MAX_ERR][128], s[160];
    cfg_defaults(&c);
    int n = cfg_load(&c, file, err, FAND_MAX_ERR);
    for (int i = 0; i < n && i < FAND_MAX_ERR; i++) printf("error: %s\n", err[i]);
    printf("FLOOR=%g HYSTERESIS=%g SLEW_UP=%g SLEW_DOWN=%g POLL_MS=%d STALE_SEC=%g SETTLE_SEC=%g START_FULL_SEC=%g\n", c.floor,
           c.hyst, c.slew_up, c.slew_down, c.poll_ms, c.stale_ms / 1000.0, c.settle_ms / 1000.0, c.start_full_ms / 1000.0);
    printf("MANUAL_TIMEOUT_SEC=%g CPU_FAN_TACH=%s TACH_MODE=%s TACH_MIN_RPM=%g TACH_ALARM_SEC=%g TACH_VERIFY_SEC=%g\n",
           c.manual_timeout_ms / 1000.0, c.cpu_tach ? "yes" : "no",
           c.tach_mode == 2 ? "accurate" : c.tach_mode == 3 ? "least" : "traditional", c.tach_min_rpm, c.tach_alarm_ms / 1000.0,
           c.tach_verify_ms / 1000.0);
    printf("TACH_VALID_DUTY=%g TACH_VALID_DUTY_OTHER=%g\n", c.tach_valid_duty, c.tach_valid_duty_other);
    printf("CPU_FAN_MIN_DUTY=%g CPU_FAN_MAX_DUTY=%g BOARD_FAN_MAX_DUTY=%g CASE_FAN_MAX_DUTY=%g EMC_WATCHDOG=%s HOST_PROBE=%s\n",
           c.cpu_fan_min, c.cpu_fan_max, c.board_fan_max, c.case_fan_max, c.emc_watchdog ? "yes" : "no",
           c.host_probe ? "yes" : "no");
    for (int z = 0; z < NZONES; z++)
        for (int g = 0; g < NGROUPS; g++)
            if (curve_key(z, g))
            {
                if (c.use[z][g]) curve_format(&c.curve[z][g], s, sizeof(s));
                else snprintf(s, sizeof(s), "none");
                printf("%s=\"%s\"\n", curve_key(z, g), s);
            }
    return n ? 1 : 0;
}

int main(int argc, char **argv)
{
    const char *cf = getenv("REIMU_FAND_CONF");
    if (cf && *cf) s_conf = cf;
    if (argc == 2 && !strcmp(argv[1], "--failsafe"))
    {
        struct config c;
        char err[FAND_MAX_ERR][128];
        cfg_defaults(&c);
        cfg_load(&c, s_conf, err, FAND_MAX_ERR);
        fand_failsafe(&c);
        fand_log(5, "fans set to 100%%");
        return 0;
    }
    if (argc >= 2 && argc <= 3 && !strcmp(argv[1], "--check-config")) return check_config(argc == 3 ? argv[2] : s_conf);
    if (argc == 2 && !strcmp(argv[1], "--status")) return print_status();
    if (argc == 2 && !strcmp(argv[1], "--no-dbus")) s_nodbus = 1;
    else if (argc == 2 && !strcmp(argv[1], "--observe")) s_observe = fand_observe = 1;
    else if (argc != 1)
    {
        fprintf(stderr, "usage: %s [--no-dbus | --observe | --failsafe | --check-config [FILE] | --status]\n", argv[0]);
        return 2;
    }
    return run_daemon();
}
