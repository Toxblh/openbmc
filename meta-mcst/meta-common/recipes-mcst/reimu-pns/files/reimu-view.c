/*
 * The web UI's view of the BMC: what its pages show, in one answer each (a
 * backend for the frontend). A page asked bmcweb for 10-20 Redfish and D-Bus
 * resources, every one a walk over D-Bus in bmcweb on a slow CPU; here the
 * same data is gathered by local D-Bus calls into one document.
 *
 *   object    /xyz/openbmc_project/reimu/view/live
 *   interface xyz.openbmc_project.Reimu.View
 *     property Json (s): what changes, read on each poll of a page:
 *       power    the platform's PowerState as Redfish says it (On, Off,
 *                PoweringOn, PoweringOff), null if unknown
 *       objects  the D-Bus objects the pages read through bmcweb's REST, by
 *                path, with the same properties (reimu sensors, fans,
 *                hostlink, overheat; the identify LED group)
 *       eth0     the BMC's network port: MACAddress, LinkStatus, SpeedMbps,
 *                IPv4Addresses (Redfish's names)
 *       sensors  every xyz.openbmc_project.Sensor.Value on D-Bus: path ->
 *                interface -> properties (value, unit, thresholds, state)
 *       static   the version of the static document: a page fetches that
 *                one again only when it changes
 *   object    /xyz/openbmc_project/reimu/view/static
 *     property Json (s): what changes at a boot or a flash write: hash (the
 *       version), objects (the ПНС document), inventory (path -> interface
 *       -> properties), bmc (FirmwareVersion, UUID as bmcweb makes it)
 *
 * Read on demand like the rest of reimu-pnsd: nothing polled in the
 * background. Every call to another service has a timeout: one that hangs
 * leaves its part null, the rest of the view comes.
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
#include <systemd/sd-id128.h>

#include "reimu-sensors.h"
#include "reimu-view.h"

#define CALL_USEC   (2 * 1000 * 1000)
#define MAX_SVC     32

static sd_bus *bus;

/* ------------------------------------------------------------------ */
/* a D-Bus message (its current position) as JSON                      */

static int basic_json(sd_bus_message *m, char type, struct buf *b)
{
    union {
        uint8_t y; int b; int16_t n; uint16_t q; int32_t i; uint32_t u; int64_t x; uint64_t t; double d; const char *s;
    } v;
    int r = sd_bus_message_read_basic(m, type, &v);
    if (r <= 0) return r;
    switch (type)
    {
    case 'y': buf_printf(b, "%u", v.y); break;
    case 'b': buf_str(b, v.b ? "true" : "false"); break;
    case 'n': buf_printf(b, "%d", v.n); break;
    case 'q': buf_printf(b, "%u", v.q); break;
    case 'i': buf_printf(b, "%d", v.i); break;
    case 'u': buf_printf(b, "%u", v.u); break;
    case 'x': buf_printf(b, "%lld", (long long)v.x); break;
    case 't': buf_printf(b, "%llu", (unsigned long long)v.t); break;
    case 'd':
        /* JSON has no NaN or infinity: a sensor with no reading has NaN */
        if (v.d != v.d || v.d > 1e308 || v.d < -1e308) buf_str(b, "null");
        else buf_printf(b, "%.17g", v.d);
        break;
    case 'h': buf_str(b, "null"); break;
    default: buf_json(b, v.s, strlen(v.s)); /* s, o, g */
    }
    return 1;
}

/* one value at the current position */
static int value_json(sd_bus_message *m, struct buf *b)
{
    char type;
    const char *contents;
    int r = sd_bus_message_peek_type(m, &type, &contents);
    if (r <= 0) return r;
    if (type == SD_BUS_TYPE_VARIANT || type == SD_BUS_TYPE_STRUCT)
    {
        r = sd_bus_message_enter_container(m, type, contents);
        if (r <= 0) return r;
        if (type == SD_BUS_TYPE_VARIANT) r = value_json(m, b);
        else
        {
            buf_str(b, "[");
            int first = 1;
            while ((r = sd_bus_message_at_end(m, 0)) == 0)
            {
                if (!first) buf_str(b, ",");
                first = 0;
                if ((r = value_json(m, b)) < 0) return r;
            }
            buf_str(b, "]");
        }
        if (r < 0) return r;
        return sd_bus_message_exit_container(m) < 0 ? -1 : 1;
    }
    if (type == SD_BUS_TYPE_ARRAY)
    {
        /* a dictionary with string keys is an object, anything else a list */
        int dict = contents[0] == '{' && (contents[1] == 's' || contents[1] == 'o');
        /* an entry's signature: "{sv}" -> "sv" */
        char entry[256];
        size_t n = strlen(contents);
        if (dict)
        {
            if (n < 4 || n - 2 >= sizeof(entry)) return -1;
            memcpy(entry, contents + 1, n - 2);
            entry[n - 2] = 0;
        }
        r = sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, contents);
        if (r <= 0) return r;
        buf_str(b, dict ? "{" : "[");
        int first = 1;
        while ((r = sd_bus_message_at_end(m, 0)) == 0)
        {
            if (!first) buf_str(b, ",");
            first = 0;
            if (dict)
            {
                if ((r = sd_bus_message_enter_container(m, SD_BUS_TYPE_DICT_ENTRY, entry)) <= 0) return -1;
                if ((r = basic_json(m, entry[0], b)) <= 0) return -1;
                buf_str(b, ":");
                if ((r = value_json(m, b)) < 0) return r;
                if (sd_bus_message_exit_container(m) < 0) return -1;
            }
            else if ((r = value_json(m, b)) < 0) return r;
        }
        buf_str(b, dict ? "}" : "]");
        return sd_bus_message_exit_container(m) < 0 ? -1 : 1;
    }
    return basic_json(m, type, b);
}

/* a method call without arguments, with a timeout; the reply or NULL */
static sd_bus_message *call(const char *dest, const char *path, const char *iface, const char *member)
{
    sd_bus_message *m = NULL, *reply = NULL;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    if (sd_bus_message_new_method_call(bus, &m, dest, path, iface, member) >= 0)
        sd_bus_call(bus, m, CALL_USEC, &err, &reply);
    sd_bus_error_free(&err);
    sd_bus_message_unref(m);
    return reply;
}

/* ObjectManager.GetManagedObjects of a service: path -> interface -> properties,
 * only the paths under PREFIX; appended without the braces (for merging) */
static int managed_json(struct buf *b, const char *dest, const char *om, const char *prefix, int first)
{
    sd_bus_message *r = call(dest, om, "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
    if (!r) return first;
    if (sd_bus_message_enter_container(r, SD_BUS_TYPE_ARRAY, "{oa{sa{sv}}}") > 0)
    {
        while (sd_bus_message_enter_container(r, SD_BUS_TYPE_DICT_ENTRY, "oa{sa{sv}}") > 0)
        {
            const char *path;
            if (sd_bus_message_read_basic(r, 'o', &path) <= 0) break;
            if (strncmp(path, prefix, strlen(prefix)))
            {
                sd_bus_message_skip(r, "a{sa{sv}}");
            }
            else
            {
                size_t at = b->len;
                if (!first) buf_str(b, ",");
                buf_json(b, path, strlen(path));
                buf_str(b, ":");
                if (value_json(r, b) <= 0)
                {
                    b->len = at;
                    if (b->s) b->s[at] = 0;
                    sd_bus_message_unref(r);
                    return first;
                }
                first = 0;
            }
            if (sd_bus_message_exit_container(r) < 0) break;
        }
    }
    sd_bus_message_unref(r);
    return first;
}

/* ------------------------------------------------------------------ */
/* the parts                                                           */

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

/* an object of another service: all the properties of its interface (what
 * bmcweb's REST gives), {} if it can't be read */
static void other_object(struct buf *b, const char *dest, const char *path, const char *iface)
{
    buf_json(b, path, strlen(path));
    buf_str(b, ":");
    sd_bus_message *m = NULL, *r = NULL;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    if (sd_bus_message_new_method_call(bus, &m, dest, path, "org.freedesktop.DBus.Properties", "GetAll") >= 0 &&
        sd_bus_message_append(m, "s", iface) >= 0)
        sd_bus_call(bus, m, CALL_USEC, &err, &r);
    sd_bus_error_free(&err);
    sd_bus_message_unref(m);
    size_t at = b->len;
    if (!r || value_json(r, b) <= 0)
    {
        b->len = at;
        if (b->s) b->s[at] = 0;
        buf_str(b, "{}");
    }
    sd_bus_message_unref(r);
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
        sd_bus_call(bus, m, CALL_USEC, &err, &r);
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
    int first = 1;
    for (int k = 0; k < n; k++)
        first = managed_json(b, svc[k], "/xyz/openbmc_project/sensors", "/xyz/openbmc_project/sensors/", first);
    sd_bus_message_unref(r);
    buf_str(b, "}");
}

/* ------------------------------------------------------------------ */
/* the static document and its version                                 */

static char *static_doc;
static uint64_t static_hash;

static uint64_t fnv(uint64_t h, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) h = (h ^ (unsigned char)s[i]) * 0x100000001b3ULL;
    return h;
}

static void bmc_json(struct buf *b)
{
    char ver[256] = "", line[256];
    FILE *f = fopen("/etc/os-release", "r");
    while (f && fgets(line, sizeof(line), f))
        if (!strncmp(line, "VERSION_ID=", 11))
        {
            char *v = line + 11;
            v[strcspn(v, "\n")] = 0;
            if (*v == '"') v++;
            v[strcspn(v, "\"")] = 0;
            snprintf(ver, sizeof(ver), "%s", v);
        }
    if (f) fclose(f);
    buf_str(b, "\"bmc\":{\"FirmwareVersion\":");
    buf_json(b, ver, strlen(ver));
    /* bmcweb's (and ipmid's) UUID of the BMC */
    sd_id128_t app = { { 0xe0, 0xe1, 0x73, 0x76, 0x64, 0x61, 0x47, 0xda, 0xa5, 0x0c, 0xd0, 0xcc, 0x64, 0x12, 0x45, 0x78 } }, id;
    if (sd_id128_get_machine_app_specific(app, &id) == 0)
    {
        char s[SD_ID128_STRING_MAX];
        sd_id128_to_string(id, s);
        buf_printf(b, ",\"UUID\":\"%.8s-%.4s-%.4s-%.4s-%.12s\"", s, s + 8, s + 12, s + 16, s + 20);
    }
    else buf_str(b, ",\"UUID\":null");
    buf_str(b, ",\"Model\":\"OpenBmc\",\"ManagerType\":\"BMC\"}");
}

/* built for each request (a few files and one call): its version is always
 * that of what it holds, a rescan of the flash shows at once */
static void build_static(void)
{
    struct buf body = { 0 };
    char *pns = pns_doc();
    buf_str(&body, "\"objects\":{");
    /* (its "now" says when it was read: not a change) */
    const char *stable = pns ? strstr(pns, ",\"fruXml\":") : NULL;
    uint64_t h = fnv(0xcbf29ce484222325ULL, stable ? stable : "", stable ? strlen(stable) : 0);
    own_object(&body, "/xyz/openbmc_project/reimu/pns", pns);
    buf_str(&body, "},\"inventory\":{");
    size_t inv = body.len;
    managed_json(&body, "xyz.openbmc_project.Inventory.Manager", "/xyz/openbmc_project/inventory",
                 "/xyz/openbmc_project/inventory/", 1);
    buf_str(&body, "},");
    bmc_json(&body);
    h = fnv(h, body.s ? body.s + inv : "", body.s ? body.len - inv : 0);
    struct buf b = { 0 };
    /* (buf_printf is for short pieces: it cuts at 511 bytes) */
    buf_printf(&b, "{\"version\":1,\"hash\":\"%016llx\",", (unsigned long long)h);
    if (body.s) buf_str(&b, body.s);
    buf_str(&b, "}");
    free(body.s);
    free(static_doc);
    static_doc = b.s;
    static_hash = h;
}

static char *live_doc(void)
{
    struct buf b = { 0 };
    buf_printf(&b, "{\"version\":1,\"now\":%lld,", (long long)time(NULL));
    power_json(&b);
    buf_str(&b, ",\"objects\":{");
    own_object(&b, "/xyz/openbmc_project/reimu/sensors", sensors_doc(0));
    buf_str(&b, ",");
    own_object(&b, "/xyz/openbmc_project/reimu/overheat", overheat_doc());
    buf_str(&b, ",");
    other_object(&b, "xyz.openbmc_project.Reimu.Fans", "/xyz/openbmc_project/reimu/fans", "xyz.openbmc_project.Reimu.Fans");
    buf_str(&b, ",");
    other_object(&b, "xyz.openbmc_project.Reimu.HostLink", "/xyz/openbmc_project/reimu/hostlink",
                 "xyz.openbmc_project.Reimu.HostLink");
    buf_str(&b, ",");
    other_object(&b, "xyz.openbmc_project.LED.GroupManager", "/xyz/openbmc_project/led/groups/enclosure_identify",
                 "xyz.openbmc_project.Led.Group");
    buf_str(&b, "},");
    eth_json(&b, "eth0");
    buf_str(&b, ",");
    sensors_dbus_json(&b);
    build_static();
    buf_printf(&b, ",\"static\":\"%016llx\"}", (unsigned long long)static_hash);
    return b.s;
}

/* ------------------------------------------------------------------ */
/* D-Bus                                                               */

static int prop_live(sd_bus *bs, const char *path, const char *iface, const char *prop, sd_bus_message *reply,
                     void *ud, sd_bus_error *err)
{
    (void)bs; (void)path; (void)iface; (void)prop; (void)ud; (void)err;
    char *j = live_doc();
    int r = sd_bus_message_append(reply, "s", j ? j : "{}");
    free(j);
    return r;
}

static int prop_static(sd_bus *bs, const char *path, const char *iface, const char *prop, sd_bus_message *reply,
                       void *ud, sd_bus_error *err)
{
    (void)bs; (void)path; (void)iface; (void)prop; (void)ud; (void)err;
    build_static();
    return sd_bus_message_append(reply, "s", static_doc ? static_doc : "{}");
}

static const sd_bus_vtable live_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("Json", "s", prop_live, 0, SD_BUS_VTABLE_PROPERTY_EMITS_INVALIDATION),
    SD_BUS_VTABLE_END
};

static const sd_bus_vtable static_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("Json", "s", prop_static, 0, SD_BUS_VTABLE_PROPERTY_EMITS_INVALIDATION),
    SD_BUS_VTABLE_END
};

int view_register(sd_bus *b)
{
    bus = b;
    int r = sd_bus_add_object_vtable(b, NULL, "/xyz/openbmc_project/reimu/view/live", "xyz.openbmc_project.Reimu.View",
                                     live_vtable, NULL);
    if (r >= 0)
        r = sd_bus_add_object_vtable(b, NULL, "/xyz/openbmc_project/reimu/view/static", "xyz.openbmc_project.Reimu.View",
                                     static_vtable, NULL);
    return r;
}

/* reimu-pnsd --view live|static: the document, for tests and a look by hand */
int view_print(sd_bus *b, const char *which)
{
    bus = b;
    char *j;
    if (!strcmp(which, "static"))
    {
        build_static();
        j = static_doc ? strdup(static_doc) : NULL;
    }
    else j = live_doc();
    if (j) puts(j);
    free(j);
    return j ? 0 : 1;
}
