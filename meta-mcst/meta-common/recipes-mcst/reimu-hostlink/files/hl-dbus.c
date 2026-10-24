/* reimu-hostlink: D-Bus (sd-bus, driven from the daemon's poll loop): the
 * service and its methods, the host's sensors as standard
 * xyz.openbmc_project.Sensor.Value objects (Redfish Chassis motherboard,
 * IPMI SDR), warning/critical events as phosphor-logging entries (Redfish
 * EventLog), and reimu-fand's Турбо followed for the GPU fan.
 *
 * Copyright (c) 2026 MCST REIMU contributors. Licensed under GPLv2.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <systemd/sd-bus.h>
#include <systemd/sd-daemon.h>

#include "hl-dbus.h"

#define SERVICE "xyz.openbmc_project.Reimu.HostLink"
#define PATH    "/xyz/openbmc_project/reimu/hostlink"
#define IFACE   "xyz.openbmc_project.Reimu.HostLink"
#define CHASSIS "/xyz/openbmc_project/inventory/system/chassis/motherboard"
#define FAND_SVC  "xyz.openbmc_project.Reimu.Fans"
#define FAND_PATH "/xyz/openbmc_project/reimu/fans"

static sd_bus *s_bus;
static struct hl *H;
static char s_spath[NSENS][96];
static double s_pub[NSENS];
static int s_alarm[NSENS][2];
static unsigned s_gen = ~0u, s_sgen = ~0u;
static int64_t s_last_emit, s_last_fand;
static sd_bus_slot *s_fand_q;
static int s_fand_missing;
static int64_t s_logged_at[8];

static int sens_of(const char *path)
{
    for (int i = 0; i < NSENS; i++)
        if (!strcmp(path, s_spath[i])) return i;
    return -1;
}

/* ------------------------------------------------------------------ service */
static int prop_root(sd_bus *bus, const char *path, const char *iface, const char *prop, sd_bus_message *reply, void *ud,
                     sd_bus_error *err)
{
    if (!strcmp(prop, "Json"))
    {
        struct buf b = { 0 };
        hl_json(H, &b);
        int r = sd_bus_message_append(reply, "s", b.s && !b.oom ? b.s : "{}");
        buf_free(&b);
        return r;
    }
    if (!strcmp(prop, "Connected")) return sd_bus_message_append(reply, "b", hl_connected(H));
    if (!strcmp(prop, "Paired")) return sd_bus_message_append(reply, "b", H->paired);
    return sd_bus_message_append(reply, "s", hl_heartbeat(H));
}

static int reply(sd_bus_message *m, sd_bus_error *err, int r, const char *e, const char *ok)
{
    if (r) return sd_bus_error_set(err, SD_BUS_ERROR_INVALID_ARGS, e);
    return sd_bus_reply_method_return(m, "s", ok);
}

static int m_fan(sd_bus_message *m, void *ud, sd_bus_error *err)
{
    const char *mode;
    double duty;
    char e[160];
    int r = sd_bus_message_read(m, "sd", &mode, &duty);
    if (r < 0) return r;
    return reply(m, err, hl_set_gpu_fan(H, mode, duty, e, sizeof(e)), e, hl_connected(H) ? "sent" : "kept (agent offline)");
}

static int m_turbo(sd_bus_message *m, void *ud, sd_bus_error *err)
{
    uint32_t min;
    char e[160];
    int r = sd_bus_message_read(m, "u", &min);
    if (r < 0) return r;
    return reply(m, err, hl_gpu_turbo(H, min > 1000 ? 1000 : (int)min, e, sizeof(e)), e, min ? "turbo on" : "turbo off");
}

static int m_power(sd_bus_message *m, void *ud, sd_bus_error *err)
{
    const char *a;
    char e[160];
    int r = sd_bus_message_read(m, "s", &a);
    if (r < 0) return r;
    return reply(m, err, hl_os_power(H, a, e, sizeof(e)), e, "sent");
}

static int m_edac(sd_bus_message *m, void *ud, sd_bus_error *err)
{
    char e[160];
    return reply(m, err, hl_edac_load(H, e, sizeof(e)), e, "sent");
}

static int m_inv(sd_bus_message *m, void *ud, sd_bus_error *err)
{
    char e[160];
    return reply(m, err, hl_request_inventory(H, e, sizeof(e)), e, "sent");
}

static int m_pair(sd_bus_message *m, void *ud, sd_bus_error *err)
{
    char e[160];
    struct buf b = { 0 };
    int r = hl_pair_begin(H, &b, e, sizeof(e));
    r = reply(m, err, r, e, b.s ? b.s : "{}");
    if (b.s) memset(b.s, 0, b.len); /* the token: not left in freed memory */
    buf_free(&b);
    return r;
}

static int m_disk(sd_bus_message *m, void *ud, sd_bus_error *err)
{
    char e[160];
    struct buf b = { 0 };
    int r = daemon_disk_start(H, &b, e, sizeof(e));
    r = reply(m, err, r, e, b.s ? b.s : "{}");
    if (b.s) memset(b.s, 0, b.len);
    buf_free(&b);
    return r;
}

static int m_disk_off(sd_bus_message *m, void *ud, sd_bus_error *err)
{
    char e[160];
    daemon_disk_stop(H, "asked", e, sizeof(e));
    return sd_bus_reply_method_return(m, "s", "off");
}

static int m_unpair(sd_bus_message *m, void *ud, sd_bus_error *err)
{
    hl_unpair(H);
    return sd_bus_reply_method_return(m, "s", "unpaired");
}

static const sd_bus_vtable root_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("Json", "s", prop_root, 0, SD_BUS_VTABLE_PROPERTY_EMITS_INVALIDATION),
    SD_BUS_PROPERTY("Connected", "b", prop_root, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("Paired", "b", prop_root, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("Heartbeat", "s", prop_root, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_METHOD("SetGpuFan", "sd", "s", m_fan, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GpuTurbo", "u", "s", m_turbo, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("OsPower", "s", "s", m_power, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("LoadEdac", "", "s", m_edac, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("RequestInventory", "", "s", m_inv, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Pair", "", "s", m_pair, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Unpair", "", "s", m_unpair, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("AgentDisk", "", "s", m_disk, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("AgentDiskOff", "", "s", m_disk_off, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END
};

/* ------------------------------------------------------------------ sensors */
static int prop_sens(sd_bus *bus, const char *path, const char *iface, const char *prop, sd_bus_message *reply, void *ud,
                     sd_bus_error *err)
{
    int i = sens_of(path);
    if (i < 0) return -ENOENT;
    const struct hl_sensor_def *d = &hl_sensors[i];
    double v = s_pub[i];
    int t = d->temp;
    if (!strcmp(prop, "Value")) return sd_bus_message_append(reply, "d", v);
    if (!strcmp(prop, "MaxValue")) return sd_bus_message_append(reply, "d", d->max);
    if (!strcmp(prop, "MinValue")) return sd_bus_message_append(reply, "d", d->min);
    if (!strcmp(prop, "Unit"))
    {
        char u[80];
        snprintf(u, sizeof(u), "xyz.openbmc_project.Sensor.Value.Unit.%s", d->unit);
        return sd_bus_message_append(reply, "s", u);
    }
    if (!strcmp(prop, "Available")) return sd_bus_message_append(reply, "b", !isnan(v));
    if (!strcmp(prop, "Functional")) return sd_bus_message_append(reply, "b", 1);
    if (!strcmp(prop, "WarningHigh")) return sd_bus_message_append(reply, "d", H->c.warn[t]);
    if (!strcmp(prop, "CriticalHigh")) return sd_bus_message_append(reply, "d", H->c.crit[t]);
    if (!strcmp(prop, "WarningLow") || !strcmp(prop, "CriticalLow")) return sd_bus_message_append(reply, "d", (double)NAN);
    if (!strcmp(prop, "WarningAlarmHigh")) return sd_bus_message_append(reply, "b", s_alarm[i][0]);
    if (!strcmp(prop, "CriticalAlarmHigh")) return sd_bus_message_append(reply, "b", s_alarm[i][1]);
    if (!strcmp(prop, "WarningAlarmLow") || !strcmp(prop, "CriticalAlarmLow")) return sd_bus_message_append(reply, "b", 0);
    if (!strcmp(prop, "Associations"))
    {
        int r = sd_bus_message_open_container(reply, 'a', "(sss)");
        if (r >= 0) r = sd_bus_message_append(reply, "(sss)", "chassis", "all_sensors", CHASSIS);
        if (r >= 0) r = sd_bus_message_close_container(reply);
        return r;
    }
    return -ENOENT;
}

#define P(name, sig) SD_BUS_PROPERTY(name, sig, prop_sens, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE)
static const sd_bus_vtable value_vt[] = { SD_BUS_VTABLE_START(0), P("Value", "d"), P("MaxValue", "d"), P("MinValue", "d"),
                                          P("Unit", "s"), SD_BUS_VTABLE_END };
static const sd_bus_vtable avail_vt[] = { SD_BUS_VTABLE_START(0), P("Available", "b"), SD_BUS_VTABLE_END };
static const sd_bus_vtable func_vt[] = { SD_BUS_VTABLE_START(0), P("Functional", "b"), SD_BUS_VTABLE_END };
static const sd_bus_vtable warn_vt[] = { SD_BUS_VTABLE_START(0), P("WarningHigh", "d"), P("WarningLow", "d"),
                                         P("WarningAlarmHigh", "b"), P("WarningAlarmLow", "b"), SD_BUS_VTABLE_END };
static const sd_bus_vtable crit_vt[] = { SD_BUS_VTABLE_START(0), P("CriticalHigh", "d"), P("CriticalLow", "d"),
                                         P("CriticalAlarmHigh", "b"), P("CriticalAlarmLow", "b"), SD_BUS_VTABLE_END };
static const sd_bus_vtable assoc_vt[] = { SD_BUS_VTABLE_START(0), P("Associations", "a(sss)"), SD_BUS_VTABLE_END };
#undef P

/* ------------------------------------------------------------------ logging */
static void on_event(struct hl *h, const struct hl_event *e)
{
    if (!s_bus || !h->c.logging || e->prio > 4) return;
    /* at most one entry per kind in 10 min, critical ones always */
    unsigned k = 0;
    for (const char *p = e->kind; *p; p++) k = k * 31 + (unsigned char)*p;
    k %= 8;
    if (e->prio > 2 && s_logged_at[k] && h->now - s_logged_at[k] < 600000) return;
    s_logged_at[k] = h->now;
    char msg[200];
    snprintf(msg, sizeof(msg), "REIMU host: %s", e->text);
    sd_bus_message *m = NULL;
    int r = sd_bus_message_new_method_call(s_bus, &m, "xyz.openbmc_project.Logging", "/xyz/openbmc_project/logging",
                                           "xyz.openbmc_project.Logging.Create", "Create");
    if (r >= 0)
        r = sd_bus_message_append(m, "ss", msg,
                                  e->prio <= 2 ? "xyz.openbmc_project.Logging.Entry.Level.Critical"
                                               : "xyz.openbmc_project.Logging.Entry.Level.Warning");
    if (r >= 0) r = sd_bus_message_open_container(m, 'a', "{ss}");
    if (r >= 0) r = sd_bus_message_append(m, "{ss}", "REIMU_HOSTLINK_EVENT", e->kind);
    if (r >= 0) r = sd_bus_message_close_container(m);
    if (r >= 0) r = sd_bus_call_async(s_bus, NULL, m, NULL, NULL, 5000000);
    sd_bus_message_unref(m);
}

/* ------------------------------------------------------------------ fand */
static int on_fand(sd_bus_message *m, void *ud, sd_bus_error *err)
{
    s_fand_q = sd_bus_slot_unref(s_fand_q);
    uint32_t left;
    if (sd_bus_message_is_method_error(m, NULL) || sd_bus_message_read(m, "v", "u", &left) < 0)
    {
        if (!s_fand_missing++)
            hl_log(6, "reimu-fand's TurboLeft not readable (%s): its Турбо not followed",
                   sd_bus_message_get_error(m) ? sd_bus_message_get_error(m)->name : "bad reply");
        return 0;
    }
    s_fand_missing = 0;
    hl_fand_turbo(H, (int)left);
    return 0;
}

void dbus_tick(struct hl *h, int64_t now)
{
    if (!s_bus || !h->c.follow_fand_turbo || s_fand_q || now - s_last_fand < 5000) return;
    s_last_fand = now;
    sd_bus_call_method_async(s_bus, &s_fand_q, FAND_SVC, FAND_PATH, "org.freedesktop.DBus.Properties", "Get", on_fand, NULL, "ss",
                             FAND_SVC, "TurboLeft");
}

/* ------------------------------------------------------------------ loop glue */
int dbus_init(struct hl *h)
{
    H = h;
    for (int i = 0; i < NSENS; i++) s_pub[i] = NAN;
    int r = sd_bus_default_system(&s_bus);
    if (r >= 0) r = sd_bus_add_object_vtable(s_bus, NULL, PATH, IFACE, root_vtable, NULL);
    for (int i = 0; r >= 0 && i < NSENS; i++)
    {
        snprintf(s_spath[i], sizeof(s_spath[i]), "/xyz/openbmc_project/sensors/%s/%s", hl_sensors[i].type, hl_sensors[i].name);
        r = sd_bus_add_object_vtable(s_bus, NULL, s_spath[i], "xyz.openbmc_project.Sensor.Value", value_vt, NULL);
        if (r >= 0)
            r = sd_bus_add_object_vtable(s_bus, NULL, s_spath[i], "xyz.openbmc_project.State.Decorator.Availability", avail_vt, NULL);
        if (r >= 0)
            r = sd_bus_add_object_vtable(s_bus, NULL, s_spath[i], "xyz.openbmc_project.State.Decorator.OperationalStatus", func_vt,
                                         NULL);
        if (r >= 0 && hl_sensors[i].temp >= 0)
            r = sd_bus_add_object_vtable(s_bus, NULL, s_spath[i], "xyz.openbmc_project.Sensor.Threshold.Warning", warn_vt, NULL);
        if (r >= 0 && hl_sensors[i].temp >= 0)
            r = sd_bus_add_object_vtable(s_bus, NULL, s_spath[i], "xyz.openbmc_project.Sensor.Threshold.Critical", crit_vt, NULL);
        if (r >= 0)
            r = sd_bus_add_object_vtable(s_bus, NULL, s_spath[i], "xyz.openbmc_project.Association.Definitions", assoc_vt, NULL);
    }
    /* the sensors are found by the mapper, read by GetManagedObjects */
    if (r >= 0) r = sd_bus_add_object_manager(s_bus, NULL, "/");
    if (r >= 0) r = sd_bus_request_name(s_bus, SERVICE, 0);
    if (r < 0)
    {
        hl_log(2, "D-Bus: %s", strerror(-r));
        return -1;
    }
    h->on_event = on_event;
    return 0;
}

int dbus_pollfd(struct pollfd *p)
{
    if (!s_bus) return 0;
    p->fd = sd_bus_get_fd(s_bus);
    int ev = sd_bus_get_events(s_bus);
    p->events = ev < 0 ? POLLIN : ev;
    p->revents = 0;
    return 1;
}

int dbus_timeout_ms(void)
{
    uint64_t us;
    if (!s_bus || sd_bus_get_timeout(s_bus, &us) < 0 || us == UINT64_MAX) return -1;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
    return us <= now ? 0 : (int)((us - now) / 1000 + 1);
}

void dbus_process(void)
{
    for (int i = 0; s_bus && i < 64; i++)
    {
        int r = sd_bus_process(s_bus, NULL);
        if (r <= 0)
        {
            if (r < 0)
            {
                /* the bus went (dbus restarted): systemd restarts us */
                hl_log(2, "D-Bus: %s", strerror(-r));
                exit(1);
            }
            break;
        }
    }
    sd_bus_flush(s_bus);
}

void dbus_publish(struct hl *h)
{
    if (!s_bus) return;
    if (h->sens_gen != s_sgen)
    {
        s_sgen = h->sens_gen;
        for (int i = 0; i < NSENS; i++)
        {
            double v = h->sens[i];
            int t = hl_sensors[i].temp;
            int wa = t >= 0 && !isnan(v) && v >= h->c.warn[t], ca = t >= 0 && !isnan(v) && v >= h->c.crit[t];
            int same = (isnan(v) && isnan(s_pub[i])) || v == s_pub[i];
            int avail = isnan(s_pub[i]) != isnan(v);
            s_pub[i] = v;
            if (!same) sd_bus_emit_properties_changed(s_bus, s_spath[i], "xyz.openbmc_project.Sensor.Value", "Value", NULL);
            if (avail)
                sd_bus_emit_properties_changed(s_bus, s_spath[i], "xyz.openbmc_project.State.Decorator.Availability", "Available",
                                               NULL);
            if (t >= 0 && wa != s_alarm[i][0])
                sd_bus_emit_properties_changed(s_bus, s_spath[i], "xyz.openbmc_project.Sensor.Threshold.Warning", "WarningAlarmHigh",
                                               NULL);
            if (t >= 0 && ca != s_alarm[i][1])
                sd_bus_emit_properties_changed(s_bus, s_spath[i], "xyz.openbmc_project.Sensor.Threshold.Critical",
                                               "CriticalAlarmHigh", NULL);
            s_alarm[i][0] = wa;
            s_alarm[i][1] = ca;
        }
    }
    /* the Json and the rest: at most once a second */
    if (h->gen != s_gen && h->now - s_last_emit >= 1000)
    {
        s_gen = h->gen;
        s_last_emit = h->now;
        sd_bus_emit_properties_changed(s_bus, PATH, IFACE, "Json", "Connected", "Paired", "Heartbeat", NULL);
    }
    sd_bus_flush(s_bus);
}

void dbus_notify(const char *state)
{
    sd_notify(0, state);
}

void dbus_close(void)
{
    sd_bus_flush_close_unref(s_bus);
    s_bus = NULL;
}
