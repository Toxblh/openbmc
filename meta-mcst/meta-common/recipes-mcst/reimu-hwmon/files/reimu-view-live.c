/*
 * The web UI's view of the BMC, its live part: what its pages show and poll,
 * in one answer (a backend for the frontend: a page asked bmcweb for 10-20
 * Redfish and D-Bus resources, every one a walk over D-Bus in bmcweb on a
 * slow CPU; here the same data is gathered by local D-Bus calls).
 *
 *   object    /xyz/openbmc_project/reimu/view/live (of reimu-hwmon)
 *   interface xyz.openbmc_project.Reimu.View
 *     property Json (s):
 *       power    the platform's PowerState as Redfish says it (On, Off,
 *                PoweringOn, PoweringOff), null if unknown
 *       objects  the D-Bus objects the pages read through bmcweb's REST, by
 *                path, with the same properties (reimu sensors, overheat,
 *                fans, hostlink; the identify LED group)
 *       eth0     the BMC's network port: MACAddress, LinkStatus, SpeedMbps,
 *                IPv4Addresses (Redfish's names)
 *       sensors  every xyz.openbmc_project.Sensor.Value on D-Bus: path ->
 *                interface -> properties (value, unit, thresholds, state)
 *       static   the version (Hash) of reimu-pnsd's view/static: a page
 *                fetches that one again only when it changes (null: unknown)
 *
 * Nothing here reads a chip: the sensors are what reimu-hwmon read last.
 * Every call to another service has a timeout: one that hangs leaves its part
 * null or empty, the rest of the view comes.
 *
 * Copyright (c) 2026 MCST REIMU contributors. Licensed under GPLv2.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <systemd/sd-bus.h>

#include "dbusjson.h"
#include "rbuf.h"
#include "reimu-overheat.h"
#include "reimu-view-live.h"
#include "sensorbus.h"

#define MAX_SVC 32

static sd_bus *bus;
static int s_daemon; /* the object is this process's */

static void power_json(struct buf *b)
{
    char *st = NULL;
    const char *p = "null";
    if (sd_bus_get_property_string(bus, "xyz.openbmc_project.State.Host", "/xyz/openbmc_project/state/host0",
                                   "xyz.openbmc_project.State.Host", "CurrentHostState", NULL, &st) >= 0 && st)
    {
        /* as bmcweb's Systems/system says it */
        const char *s = strrchr(st, '.');
        s = s ? s + 1 : st;
        p = !strcmp(s, "Running") || !strcmp(s, "Quiesced") || !strcmp(s, "DiagnosticMode") ? "\"On\""
            : !strcmp(s, "TransitioningToRunning")                                          ? "\"PoweringOn\""
            : !strcmp(s, "TransitioningToOff")                                              ? "\"PoweringOff\""
                                                                                            : "\"Off\"";
    }
    free(st);
    buf_printf(b, "\"power\":%s", p);
}

/* an object of our own process, as bmcweb's REST gives it: {"Json": "..."} */
static void own_object(struct buf *b, const char *path, char *doc)
{
    buf_json(b, path, strlen(path));
    buf_str(b, ":{\"Json\":");
    if (doc) buf_json(b, doc, strlen(doc));
    else buf_str(b, "null");
    buf_str(b, "}");
    free(doc);
}

static int read_line(const char *path, char *out, size_t n)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char *s = fgets(out, n, f);
    fclose(f);
    if (!s) return -1;
    out[strcspn(out, "\n")] = 0;
    return 0;
}

/* the BMC's network port, by Redfish's names */
static void eth_json(struct buf *b, const char *ifname)
{
    char p[128], v[64];
    buf_printf(b, "\"%s\":{", ifname);
    snprintf(p, sizeof(p), "/sys/class/net/%s/address", ifname);
    buf_str(b, "\"MACAddress\":");
    if (!read_line(p, v, sizeof(v))) buf_json(b, v, strlen(v));
    else buf_str(b, "null");
    snprintf(p, sizeof(p), "/sys/class/net/%s/carrier", ifname);
    int up = !read_line(p, v, sizeof(v)) && !strcmp(v, "1");
    buf_printf(b, ",\"LinkStatus\":\"%s\"", up ? "LinkUp" : "LinkDown");
    snprintf(p, sizeof(p), "/sys/class/net/%s/speed", ifname);
    long speed = up && !read_line(p, v, sizeof(v)) ? atol(v) : 0;
    if (speed > 0) buf_printf(b, ",\"SpeedMbps\":%ld", speed);
    else buf_str(b, ",\"SpeedMbps\":null");
    buf_str(b, ",\"IPv4Addresses\":[");
    struct ifaddrs *ifa = NULL, *i;
    int first = 1;
    if (getifaddrs(&ifa) == 0)
        for (i = ifa; i; i = i->ifa_next)
        {
            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || strcmp(i->ifa_name, ifname)) continue;
            char a[INET_ADDRSTRLEN], m[INET_ADDRSTRLEN] = "";
            inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, a, sizeof(a));
            if (i->ifa_netmask) inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_netmask)->sin_addr, m, sizeof(m));
            buf_printf(b, "%s{\"Address\":\"%s\",\"SubnetMask\":\"%s\"}", first ? "" : ",", a, m);
            first = 0;
        }
    if (ifa) freeifaddrs(ifa);
    buf_str(b, "]}");
}

/* every Sensor.Value on D-Bus: the mapper names the services, one
 * GetManagedObjects each */
static void sensors_dbus_json(struct buf *b)
{
    buf_str(b, "\"sensors\":{");
    const char *svc[MAX_SVC];
    int n = 0;
    sd_bus_message *m = NULL, *r = NULL;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    if (sd_bus_message_new_method_call(bus, &m, "xyz.openbmc_project.ObjectMapper", "/xyz/openbmc_project/object_mapper",
                                       "xyz.openbmc_project.ObjectMapper", "GetSubTree") >= 0 &&
        sd_bus_message_append(m, "sias", "/xyz/openbmc_project/sensors", 0, 1, "xyz.openbmc_project.Sensor.Value") >= 0)
        sd_bus_call(bus, m, DJ_CALL_USEC, &err, &r);
    sd_bus_error_free(&err);
    sd_bus_message_unref(m);
    /* a{sa{sas}}: path -> service -> interfaces */
    if (r && sd_bus_message_enter_container(r, SD_BUS_TYPE_ARRAY, "{sa{sas}}") > 0)
    {
        while (sd_bus_message_enter_container(r, SD_BUS_TYPE_DICT_ENTRY, "sa{sas}") > 0)
        {
            sd_bus_message_skip(r, "s");
            if (sd_bus_message_enter_container(r, SD_BUS_TYPE_ARRAY, "{sas}") > 0)
            {
                while (sd_bus_message_enter_container(r, SD_BUS_TYPE_DICT_ENTRY, "sas") > 0)
                {
                    const char *s;
                    if (sd_bus_message_read_basic(r, 's', &s) > 0)
                    {
                        int k;
                        for (k = 0; k < n && strcmp(svc[k], s); k++) {}
                        if (k == n && n < MAX_SVC) svc[n++] = s; /* (the reply lives till the end) */
                    }
                    sd_bus_message_skip(r, "as");
                    sd_bus_message_exit_container(r);
                }
                sd_bus_message_exit_container(r);
            }
            sd_bus_message_exit_container(r);
        }
    }
    /* the daemon's own: from its table (a call to itself would wait for
     * itself); reimu-hwmon --view asks the daemon as any other */
    int first = s_daemon ? sb_json(b, 1) : 1;
    for (int k = 0; k < n; k++)
        if (!s_daemon || strcmp(svc[k], VIEW_SELF))
            first = dj_managed(bus, b, svc[k], "/xyz/openbmc_project/sensors", "/xyz/openbmc_project/sensors/", first);
    sd_bus_message_unref(r);
    buf_str(b, "}");
}

/* reimu-pnsd's view/static: its version */
static void static_hash(struct buf *b)
{
    char *h = NULL;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *m = NULL, *r = NULL;
    if (sd_bus_message_new_method_call(bus, &m, "xyz.openbmc_project.Reimu.Pns", "/xyz/openbmc_project/reimu/view/static",
                                       "org.freedesktop.DBus.Properties", "Get") >= 0 &&
        sd_bus_message_append(m, "ss", "xyz.openbmc_project.Reimu.View", "Hash") >= 0)
        sd_bus_call(bus, m, DJ_CALL_USEC, &err, &r);
    const char *s;
    if (r && sd_bus_message_read(r, "v", "s", &s) > 0) h = strdup(s);
    sd_bus_error_free(&err);
    sd_bus_message_unref(m);
    sd_bus_message_unref(r);
    buf_str(b, ",\"static\":");
    if (h) buf_json(b, h, strlen(h));
    else buf_str(b, "null");
    free(h);
}

char *view_live_doc(void)
{
    struct buf b = { 0 };
    buf_printf(&b, "{\"version\":1,\"now\":%lld,", (long long)time(NULL));
    power_json(&b);
    buf_str(&b, ",\"objects\":{");
    own_object(&b, "/xyz/openbmc_project/reimu/sensors", sensors_doc());
    buf_str(&b, ",");
    own_object(&b, "/xyz/openbmc_project/reimu/overheat", overheat_doc());
    buf_str(&b, ",");
    dj_object(bus, &b, "xyz.openbmc_project.Reimu.Fans", "/xyz/openbmc_project/reimu/fans", "xyz.openbmc_project.Reimu.Fans");
    buf_str(&b, ",");
    dj_object(bus, &b, "xyz.openbmc_project.Reimu.HostLink", "/xyz/openbmc_project/reimu/hostlink",
              "xyz.openbmc_project.Reimu.HostLink");
    buf_str(&b, ",");
    dj_object(bus, &b, "xyz.openbmc_project.LED.GroupManager", "/xyz/openbmc_project/led/groups/enclosure_identify",
              "xyz.openbmc_project.Led.Group");
    buf_str(&b, "},");
    eth_json(&b, "eth0");
    buf_str(&b, ",");
    sensors_dbus_json(&b);
    static_hash(&b);
    buf_str(&b, "}");
    return b.s;
}

static int prop_live(sd_bus *bs, const char *path, const char *iface, const char *prop, sd_bus_message *reply,
                     void *ud, sd_bus_error *err)
{
    (void)bs; (void)path; (void)iface; (void)prop; (void)ud; (void)err;
    char *j = view_live_doc();
    int r = sd_bus_message_append(reply, "s", j ? j : "{}");
    free(j);
    return r;
}

static const sd_bus_vtable live_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("Json", "s", prop_live, 0, SD_BUS_VTABLE_PROPERTY_EMITS_INVALIDATION),
    SD_BUS_VTABLE_END
};

int view_live_register(sd_bus *b)
{
    bus = b;
    s_daemon = 1;
    return sd_bus_add_object_vtable(b, NULL, "/xyz/openbmc_project/reimu/view/live", "xyz.openbmc_project.Reimu.View",
                                    live_vtable, NULL);
}

void view_live_bus(sd_bus *b)
{
    bus = b;
}
