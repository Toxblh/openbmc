/*
 * reimu-hwmon: the platform sensors of REIMU-5564 (1Э8СВ-uATX), each read
 * once and given to everyone: Redfish and IPMI (standard Sensor.Value
 * objects), the web UI (its documents), Prometheus (Metrics). Nothing that
 * asks reads a chip: it gets what was read last.
 *
 *   service   xyz.openbmc_project.Reimu.Hwmon
 *   objects   /xyz/openbmc_project/sensors/<type>/<name>   sensorbus.c
 *             /xyz/openbmc_project/reimu/sensors
 *               interface xyz.openbmc_project.Reimu.Sensors
 *               property  Json (s): the web UI's document of the sensors,
 *                         see sensors_doc() and hwmon-model.c
 *             /xyz/openbmc_project/reimu/hwmon
 *               interface xyz.openbmc_project.Reimu.Hwmon
 *               property  Metrics (s): the Prometheus text format
 *             /xyz/openbmc_project/reimu/overheat          reimu-overheat.c
 *             /xyz/openbmc_project/reimu/view/live         reimu-view-live.c
 *
 * Who reads what (one reader per chip, see hwmon-model.c): reimu-fand reads
 * the chips it drives (lm96163, emc2305) and the inputs of its control (the
 * PCS, the VRMs' temperatures) and hands them over (its Readings, asked for
 * every POLL_MS); this daemon reads the rest (hwmon-chips.c: the PMBus
 * devices' rails, the jc42 DIMM sensors), the devices from the ПНС device
 * tree (hwmon-dt.c). The kernel drivers of these chips aren't bound (no
 * hwmon-instantiator, no phosphor-hwmon): nothing else polls them.
 *
 * Only while the platform runs (CurrentHostState Running, SETTLE_SEC after it
 * came: its power-up must not see BMC traffic on the bus; since when it runs
 * is kept in /run/reimu-hwmon/running-since over a restart. Not reimu-fand's
 * host-on file: that one also says on when its probe of the chips answers with
 * the host state Off). Off: no sensors, no transfers.
 *
 *   reimu-hwmon               the daemon
 *   reimu-hwmon --sensors     the web UI's document, read now (no power check)
 *   reimu-hwmon --metrics     the same as Prometheus text
 *   reimu-hwmon --view live   the web UI's live view, through the daemon
 *
 * REIMU_DTB: the device tree (default /var/volatile/motherboard_devtree.dtb),
 * REIMU_PCS_BUS (5), REIMU_HWMON_POLL_MS (3000), REIMU_HWMON_SETTLE_SEC (30),
 * REIMU_HWMON_RUN (/run/reimu-hwmon), REIMU_HWMON_FAKE (hwmon-chips.c).
 *
 * Copyright (c) 2026 MCST REIMU contributors. Licensed under GPLv2.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>

#include "hwmon.h"
#include "rbuf.h"
#include "reimu-overheat.h"
#include "reimu-view-live.h"
#include "sensorbus.h"

#define SERVICE   "xyz.openbmc_project.Reimu.Hwmon"
#define HOST_SVC  "xyz.openbmc_project.State.Host"
#define HOST_PATH "/xyz/openbmc_project/state/host0"
#define FAND_SVC  "xyz.openbmc_project.Reimu.Fans"
#define FAND_PATH "/xyz/openbmc_project/reimu/fans"
#define DTB_FILE  "/var/volatile/motherboard_devtree.dtb"
#define QUERY_MS  5000
/* reimu-fand's Readings older than this: not used */
#define FAND_MAX_AGE_MS 10000

static sd_bus *s_bus;
static sd_event *s_ev;
static int s_poll_ms = 3000, s_settle_ms = 30000, s_pcs_bus = 5;
static const char *s_dtb = DTB_FILE;
static char s_runfile[256];

static struct catalog s_cat;
static int s_cat_ok;
static struct timespec s_cat_mtime;

static struct reading s_fand[MAX_READINGS], s_own[MAX_READINGS];
static int s_nfand, s_nown;
static int64_t s_fand_at;
static sd_bus_slot *s_fand_q;
static int s_fand_missing;

static struct entry s_ent[MAX_ENTRIES];
static int s_nent;

/* host power: -1 unknown, 0 off, 1 running; since when (ms) */
static int s_host = -1;
static int64_t s_running_since, s_last_query = -QUERY_MS;
static sd_bus_slot *s_host_q;

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void hlog(int prio, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void hlog(int prio, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "<%d>", prio);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* ------------------------------------------------------------------ platform */
/* since when the host has been running (CLOCK_MONOTONIC ms), kept over a
 * restart of this daemon; -1 none */
static int64_t saved_since(void)
{
    char t[32];
    int fd = open(s_runfile, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = read(fd, t, sizeof(t) - 1);
    close(fd);
    if (n <= 0) return -1;
    t[n] = 0;
    char *e;
    long long v = strtoll(t, &e, 10);
    return e != t && v >= 0 ? v : -1;
}

static void save_since(int64_t v)
{
    if (v < 0)
    {
        unlink(s_runfile);
        return;
    }
    char tmp[300], t[32];
    snprintf(tmp, sizeof(tmp), "%s.new", s_runfile);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return;
    int len = snprintf(t, sizeof(t), "%lld\n", (long long)v);
    int ok = write(fd, t, len) == len;
    close(fd);
    if (ok) rename(tmp, s_runfile);
    else unlink(tmp);
}

static void set_host(const char *st)
{
    int on = strstr(st, ".Running") != NULL;
    if (on && s_host != 1)
    {
        /* the first answer after a restart: since when it was seen running */
        int64_t now = now_ms(), f = s_host == -1 ? saved_since() : -1;
        s_running_since = f >= 0 && f <= now ? f : now;
        save_since(s_running_since);
    }
    if (!on && s_host != 0) save_since(-1);
    if (on != s_host) hlog(6, "host state %s", st);
    s_host = on;
}

/* "off", "starting" (SETTLE_SEC not over), "on" */
static const char *platform(int64_t now)
{
    if (s_host != 1) return "off";
    return now - s_running_since < s_settle_ms ? "starting" : "on";
}

static int on_props(sd_bus_message *m, void *ud, sd_bus_error *err)
{
    const char *iface;
    (void)ud; (void)err;
    if (sd_bus_message_read(m, "s", &iface) < 0 || strcmp(iface, HOST_SVC)) return 0;
    if (sd_bus_message_enter_container(m, 'a', "{sv}") < 0) return 0;
    while (sd_bus_message_enter_container(m, 'e', "sv") > 0)
    {
        const char *name, *val;
        if (sd_bus_message_read(m, "s", &name) < 0) return 0;
        if (!strcmp(name, "CurrentHostState") && sd_bus_message_read(m, "v", "s", &val) >= 0) set_host(val);
        else sd_bus_message_skip(m, "v");
        sd_bus_message_exit_container(m);
    }
    return 0;
}

static int on_host(sd_bus_message *m, void *ud, sd_bus_error *err)
{
    const char *val;
    (void)ud; (void)err;
    s_host_q = sd_bus_slot_unref(s_host_q);
    if (sd_bus_message_is_method_error(m, NULL) || sd_bus_message_read(m, "v", "s", &val) < 0)
    {
        if (s_host != -1) hlog(4, "host state unreadable: no platform sensors");
        s_host = -1;
        return 0;
    }
    set_host(val);
    return 0;
}

static void query_host(int64_t now)
{
    if (s_host_q || now - s_last_query < QUERY_MS) return;
    s_last_query = now;
    sd_bus_call_method_async(s_bus, &s_host_q, HOST_SVC, HOST_PATH, "org.freedesktop.DBus.Properties", "Get", on_host, NULL,
                             "ss", HOST_SVC, "CurrentHostState");
}

/* ------------------------------------------------------------------ readings */
static void load_catalog(void)
{
    struct stat st;
    int have = stat(s_dtb, &st) == 0;
    if (have && s_cat_ok && st.st_mtim.tv_sec == s_cat_mtime.tv_sec && st.st_mtim.tv_nsec == s_cat_mtime.tv_nsec) return;
    if (!have && !s_cat_ok) return;
    s_cat_ok = have && catalog_load(&s_cat, s_dtb, s_pcs_bus) == 0;
    if (have) s_cat_mtime = st.st_mtim;
    if (s_cat_ok) hlog(6, "device tree %s: %d devices with sensors", s_dtb, s_cat.n);
    else hlog(4, "no device tree (%s): no platform sensors", s_dtb);
}

/* a(ssddt) of reimu-fand's Readings into r */
static int parse_readings(sd_bus_message *m, struct reading *r, int max)
{
    int n = 0;
    if (sd_bus_message_enter_container(m, 'v', "a(ssddt)") <= 0 || sd_bus_message_enter_container(m, 'a', "(ssddt)") <= 0)
        return -1;
    const char *dev, *in;
    double v, duty;
    uint64_t at;
    while (n < max && sd_bus_message_read(m, "(ssddt)", &dev, &in, &v, &duty, &at) > 0)
    {
        snprintf(r[n].dev, sizeof(r[n].dev), "%s", dev);
        snprintf(r[n].input, sizeof(r[n].input), "%s", in);
        r[n].value = v;
        r[n].duty = duty;
        r[n].at = (int64_t)(at / 1000);
        n++;
    }
    return n;
}

static void rebuild(int64_t now)
{
    static struct reading all[2 * MAX_READINGS];
    int n = 0;
    s_nent = 0;
    if (strcmp(platform(now), "on") || !s_cat_ok)
    {
        sb_update(NULL, 0);
        return;
    }
    if (s_fand_at && now - s_fand_at <= FAND_MAX_AGE_MS)
        for (int i = 0; i < s_nfand; i++) all[n++] = s_fand[i];
    for (int i = 0; i < s_nown; i++) all[n++] = s_own[i];
    s_nent = model_build(&s_cat, all, n, s_ent, MAX_ENTRIES);
    sb_update(s_ent, s_nent);
}

static int on_fand(sd_bus_message *m, void *ud, sd_bus_error *err)
{
    (void)ud; (void)err;
    s_fand_q = sd_bus_slot_unref(s_fand_q);
    int n = sd_bus_message_is_method_error(m, NULL) ? -1 : parse_readings(m, s_fand, MAX_READINGS);
    if (n < 0)
    {
        if (!s_fand_missing++)
            hlog(5, "reimu-fand's Readings not there (%s): no fans' or PCS sensors",
                 sd_bus_message_get_error(m) ? sd_bus_message_get_error(m)->name : "bad reply");
        s_nfand = 0;
        return 0;
    }
    if (s_fand_missing) hlog(6, "reimu-fand's Readings back");
    s_fand_missing = 0;
    s_nfand = n;
    s_fand_at = now_ms();
    rebuild(s_fand_at);
    return 0;
}

static void ask_fand(void)
{
    if (s_fand_q) return;
    sd_bus_call_method_async(s_bus, &s_fand_q, FAND_SVC, FAND_PATH, "org.freedesktop.DBus.Properties", "Get", on_fand, NULL,
                             "ss", FAND_SVC, "Readings");
}

static void read_own(int64_t now)
{
    s_nown = 0;
    if (strcmp(platform(now), "on") || !s_cat_ok) return;
    for (int i = 0; i < s_cat.n; i++) s_nown = chips_read(&s_cat.d[i], s_own, s_nown, MAX_READINGS, now);
}

/* ------------------------------------------------------------------ documents */
char *sensors_doc(void)
{
    struct buf b = { 0 };
    int64_t now = now_ms();
    const char *p = platform(now);
    buf_printf(&b, "{\"version\":1,\"now\":%lld,\"platform\":\"%s\",", (long long)time(NULL), p);
    /* the BMC's own load: there with the host off as well */
    double la[3];
    FILE *lf = fopen("/proc/loadavg", "r");
    if (lf && fscanf(lf, "%lf %lf %lf", &la[0], &la[1], &la[2]) == 3)
        buf_printf(&b, "\"bmc\":{\"load\":[%.2f,%.2f,%.2f]},", la[0], la[1], la[2]);
    if (lf) fclose(lf);
    if (!strcmp(p, "on") && !s_cat_ok) buf_str(&b, "\"sensors\":[],\"error\":\"no device tree\"");
    else model_json(s_ent, strcmp(p, "on") ? 0 : s_nent, &b);
    buf_str(&b, "}");
    return b.s;
}

/* /proc/meminfo's value of key (kB), -1 */
static long meminfo(const char *key)
{
    char line[128];
    long v = -1;
    size_t k = strlen(key);
    FILE *f = fopen("/proc/meminfo", "r");
    while (f && fgets(line, sizeof(line), f))
        if (!strncmp(line, key, k) && line[k] == ':') v = atol(line + k + 1);
    if (f) fclose(f);
    return v;
}

static char *metrics_doc(void)
{
    struct buf b = { 0 };
    int64_t now = now_ms();
    const char *p = platform(now);
    buf_printf(&b, "# HELP reimu_platform_state The platform: 0 off, 1 starting (the bus left alone), 2 on\n"
                   "# TYPE reimu_platform_state gauge\nreimu_platform_state %d\n",
               !strcmp(p, "on") ? 2 : !strcmp(p, "starting") ? 1 : 0);
    double la[3], up;
    FILE *f = fopen("/proc/loadavg", "r");
    if (f && fscanf(f, "%lf %lf %lf", &la[0], &la[1], &la[2]) == 3)
        buf_printf(&b, "# HELP reimu_bmc_load1 The BMC's load average, 1 min\n# TYPE reimu_bmc_load1 gauge\nreimu_bmc_load1 %.2f\n"
                       "# HELP reimu_bmc_load5 The BMC's load average, 5 min\n# TYPE reimu_bmc_load5 gauge\nreimu_bmc_load5 %.2f\n",
                   la[0], la[1]);
    if (f) fclose(f);
    f = fopen("/proc/uptime", "r");
    if (f && fscanf(f, "%lf", &up) == 1)
        buf_printf(&b, "# HELP reimu_bmc_uptime_seconds The BMC's uptime\n# TYPE reimu_bmc_uptime_seconds counter\n"
                       "reimu_bmc_uptime_seconds %.0f\n", up);
    if (f) fclose(f);
    long avail = meminfo("MemAvailable"), total = meminfo("MemTotal");
    if (avail >= 0 && total > 0)
        buf_printf(&b, "# HELP reimu_bmc_memory_bytes The BMC's memory\n# TYPE reimu_bmc_memory_bytes gauge\n"
                       "reimu_bmc_memory_bytes{kind=\"total\"} %ld\nreimu_bmc_memory_bytes{kind=\"available\"} %ld\n",
                   total * 1024, avail * 1024);
    buf_printf(&b, "# HELP reimu_hwmon_transfers_total I2C transfers reimu-hwmon made\n# TYPE reimu_hwmon_transfers_total counter\n"
                   "reimu_hwmon_transfers_total %lu\n", hw_transfers());
    if (!strcmp(p, "on")) model_metrics(s_ent, s_nent, &b);
    return b.s;
}

static int prop_sensors(sd_bus *bus, const char *path, const char *iface, const char *prop, sd_bus_message *reply,
                        void *ud, sd_bus_error *err)
{
    (void)bus; (void)path; (void)iface; (void)prop; (void)ud; (void)err;
    char *j = sensors_doc();
    int r = sd_bus_message_append(reply, "s", j ? j : "{}");
    free(j);
    return r;
}

static int prop_metrics(sd_bus *bus, const char *path, const char *iface, const char *prop, sd_bus_message *reply,
                        void *ud, sd_bus_error *err)
{
    (void)bus; (void)path; (void)iface; (void)prop; (void)ud; (void)err;
    char *j = metrics_doc();
    int r = sd_bus_message_append(reply, "s", j ? j : "");
    free(j);
    return r;
}

static const sd_bus_vtable sensors_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("Json", "s", prop_sensors, 0, SD_BUS_VTABLE_PROPERTY_EMITS_INVALIDATION),
    SD_BUS_VTABLE_END
};

static const sd_bus_vtable hwmon_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("Metrics", "s", prop_metrics, 0, SD_BUS_VTABLE_PROPERTY_EMITS_INVALIDATION),
    SD_BUS_VTABLE_END
};

/* ------------------------------------------------------------------ loop */
static int on_tick(sd_event_source *s, uint64_t usec, void *ud)
{
    (void)ud;
    int64_t now = now_ms();
    query_host(now);
    load_catalog();
    ask_fand();
    read_own(now);
    rebuild(now);
    sd_event_source_set_time(s, usec + (uint64_t)s_poll_ms * 1000);
    sd_event_source_set_enabled(s, SD_EVENT_ONESHOT);
    return 0;
}

static int on_signal(sd_event_source *s, const struct signalfd_siginfo *si, void *ud)
{
    (void)s; (void)si; (void)ud;
    sd_event_exit(s_ev, 0);
    return 0;
}

static int env_int(const char *name, int def, int lo, int hi)
{
    const char *v = getenv(name);
    if (!v || !*v) return def;
    long x = strtol(v, NULL, 10);
    return x < lo ? lo : x > hi ? hi : (int)x;
}

static void setup(void)
{
    const char *d = getenv("REIMU_DTB");
    if (d && *d) s_dtb = d;
    s_pcs_bus = env_int("REIMU_PCS_BUS", 5, 0, 15);
    s_poll_ms = env_int("REIMU_HWMON_POLL_MS", 3000, 200, 60000);
    s_settle_ms = env_int("REIMU_HWMON_SETTLE_SEC", 30, 0, 600) * 1000;
    const char *rd = getenv("REIMU_HWMON_RUN");
    rd = rd && *rd ? rd : "/run/reimu-hwmon";
    mkdir(rd, 0755);
    snprintf(s_runfile, sizeof(s_runfile), "%s/running-since", rd);
}

static int run_daemon(void)
{
    sigset_t ss;
    sigemptyset(&ss);
    sigaddset(&ss, SIGTERM);
    sigaddset(&ss, SIGINT);
    sigprocmask(SIG_BLOCK, &ss, NULL);
    int r = sd_event_default(&s_ev);
    if (r >= 0) r = sd_bus_default_system(&s_bus);
    if (r < 0)
    {
        hlog(2, "D-Bus: %s", strerror(-r));
        return 1;
    }
    sd_event_add_signal(s_ev, NULL, SIGTERM, on_signal, NULL);
    sd_event_add_signal(s_ev, NULL, SIGINT, on_signal, NULL);
    sb_init(s_bus);
    r = sd_bus_add_object_vtable(s_bus, NULL, "/xyz/openbmc_project/reimu/sensors", "xyz.openbmc_project.Reimu.Sensors",
                                 sensors_vtable, NULL);
    if (r >= 0)
        r = sd_bus_add_object_vtable(s_bus, NULL, "/xyz/openbmc_project/reimu/hwmon", "xyz.openbmc_project.Reimu.Hwmon",
                                     hwmon_vtable, NULL);
    /* the switch of overheatd's power actions, the reason of an emergency */
    if (r >= 0) r = overheat_register(s_bus);
    /* what the web UI's pages poll, in one answer */
    if (r >= 0) r = view_live_register(s_bus);
    /* the sensors are found by the mapper, read by GetManagedObjects */
    if (r >= 0) r = sd_bus_add_object_manager(s_bus, NULL, "/");
    if (r >= 0)
        r = sd_bus_match_signal_async(s_bus, NULL, NULL, HOST_PATH, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                                      on_props, NULL, NULL);
    if (r >= 0) r = sd_bus_attach_event(s_bus, s_ev, SD_EVENT_PRIORITY_NORMAL);
    if (r >= 0) r = sd_bus_request_name(s_bus, SERVICE, 0);
    sd_event_source *tick = NULL;
    if (r >= 0) r = sd_event_add_time(s_ev, &tick, CLOCK_MONOTONIC, 0, 100000, on_tick, NULL);
    if (r < 0)
    {
        hlog(2, "D-Bus setup failed: %s", strerror(-r));
        return 1;
    }
    load_catalog();
    hlog(5, "started: every %d ms while the platform runs (%d s after it came)", s_poll_ms, s_settle_ms / 1000);
    r = sd_event_loop(s_ev);
    sb_update(NULL, 0);
    sd_bus_flush_close_unref(s_bus);
    sd_event_unref(s_ev);
    return r < 0 ? 1 : 0;
}

/* --sensors, --metrics: read now, whatever the power (by hand, the tests) */
static int once(int metrics)
{
    int64_t now = now_ms();
    s_host = 1;
    s_running_since = now - s_settle_ms;
    load_catalog();
    if (sd_bus_default_system(&s_bus) >= 0)
    {
        sd_bus_message *m = NULL;
        if (sd_bus_get_property(s_bus, FAND_SVC, FAND_PATH, FAND_SVC, "Readings", NULL, &m, "a(ssddt)") >= 0)
        {
            /* (positioned at the value: the variant entered) */
            const char *dev, *in;
            double v, duty;
            uint64_t at;
            if (sd_bus_message_enter_container(m, 'a', "(ssddt)") > 0)
                while (s_nfand < MAX_READINGS && sd_bus_message_read(m, "(ssddt)", &dev, &in, &v, &duty, &at) > 0)
                {
                    struct reading *x = &s_fand[s_nfand++];
                    snprintf(x->dev, sizeof(x->dev), "%s", dev);
                    snprintf(x->input, sizeof(x->input), "%s", in);
                    x->value = v;
                    x->duty = duty;
                    x->at = (int64_t)(at / 1000);
                }
            s_fand_at = now;
        }
        sd_bus_message_unref(m);
    }
    read_own(now);
    rebuild(now);
    char *j = metrics ? metrics_doc() : sensors_doc();
    if (j) fputs(j, stdout);
    if (j && !metrics) fputc('\n', stdout);
    free(j);
    sd_bus_unref(s_bus);
    return 0;
}

int main(int argc, char **argv)
{
    setup();
    if (argc == 2 && !strcmp(argv[1], "--sensors")) return once(0);
    if (argc == 2 && !strcmp(argv[1], "--metrics")) return once(1);
    if (argc == 3 && !strcmp(argv[1], "--view") && !strcmp(argv[2], "live"))
    {
        if (sd_bus_default_system(&s_bus) < 0) return 1;
        view_live_bus(s_bus);
        char *j = view_live_doc();
        if (j) puts(j);
        free(j);
        sd_bus_unref(s_bus);
        return j ? 0 : 1;
    }
    if (argc == 1) return run_daemon();
    fprintf(stderr, "usage: %s [--sensors | --metrics | --view live]\n", argv[0]);
    return 2;
}
