/*
 * reimu-hwmon's sensors as standard D-Bus objects, for Redfish (bmcweb's
 * Chassis sensors) and IPMI (the SDR config reads Value of these paths):
 *
 *   /xyz/openbmc_project/sensors/<type>/<name>
 *     xyz.openbmc_project.Sensor.Value                     Value, MaxValue, MinValue, Unit
 *     xyz.openbmc_project.State.Decorator.Availability     Available (true)
 *     xyz.openbmc_project.State.Decorator.OperationalStatus Functional (true)
 *     xyz.openbmc_project.Association.Definitions          chassis -> all_sensors of SB_CHASSIS
 *
 * As phosphor-hwmon did: a sensor is there only while it reads (no object for
 * one that doesn't: Redfish would list it without a reading, IPMI read NaN),
 * none while the platform is off. Found by the mapper through the
 * InterfacesAdded/Removed of the ObjectManager at "/".
 *
 * Copyright (c) 2026 MCST REIMU contributors. Licensed under GPLv2.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sensorbus.h"

struct pub
{
    char path[160];
    sd_bus_slot *slot[4];
    double value, min, max;
    char unit[80];
    int used;
};

static sd_bus *s_bus;
static struct pub s_pub[MAX_ENTRIES];

static const char *const IFACES[4] = {
    "xyz.openbmc_project.Sensor.Value",
    "xyz.openbmc_project.State.Decorator.Availability",
    "xyz.openbmc_project.State.Decorator.OperationalStatus",
    "xyz.openbmc_project.Association.Definitions",
};

static int prop(sd_bus *bus, const char *path, const char *iface, const char *name, sd_bus_message *reply, void *ud,
                sd_bus_error *err)
{
    const struct pub *p = ud;
    (void)bus; (void)path; (void)iface; (void)err;
    if (!strcmp(name, "Value")) return sd_bus_message_append(reply, "d", p->value);
    if (!strcmp(name, "MaxValue")) return sd_bus_message_append(reply, "d", p->max);
    if (!strcmp(name, "MinValue")) return sd_bus_message_append(reply, "d", p->min);
    if (!strcmp(name, "Unit")) return sd_bus_message_append(reply, "s", p->unit);
    if (!strcmp(name, "Available") || !strcmp(name, "Functional")) return sd_bus_message_append(reply, "b", 1);
    if (!strcmp(name, "Associations"))
    {
        int r = sd_bus_message_open_container(reply, 'a', "(sss)");
        if (r >= 0) r = sd_bus_message_append(reply, "(sss)", "chassis", "all_sensors", SB_CHASSIS);
        if (r >= 0) r = sd_bus_message_close_container(reply);
        return r;
    }
    return -ENOENT;
}

#define P(name, sig, flags) SD_BUS_PROPERTY(name, sig, prop, 0, flags)
static const sd_bus_vtable value_vt[] = { SD_BUS_VTABLE_START(0), P("Value", "d", SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
                                          P("MaxValue", "d", SD_BUS_VTABLE_PROPERTY_CONST),
                                          P("MinValue", "d", SD_BUS_VTABLE_PROPERTY_CONST),
                                          P("Unit", "s", SD_BUS_VTABLE_PROPERTY_CONST), SD_BUS_VTABLE_END };
static const sd_bus_vtable avail_vt[] = { SD_BUS_VTABLE_START(0), P("Available", "b", SD_BUS_VTABLE_PROPERTY_CONST),
                                          SD_BUS_VTABLE_END };
static const sd_bus_vtable func_vt[] = { SD_BUS_VTABLE_START(0), P("Functional", "b", SD_BUS_VTABLE_PROPERTY_CONST),
                                         SD_BUS_VTABLE_END };
static const sd_bus_vtable assoc_vt[] = { SD_BUS_VTABLE_START(0), P("Associations", "a(sss)", SD_BUS_VTABLE_PROPERTY_CONST),
                                          SD_BUS_VTABLE_END };
#undef P
static const sd_bus_vtable *const VTABLES[4] = { value_vt, avail_vt, func_vt, assoc_vt };

void sb_init(sd_bus *bus)
{
    s_bus = bus;
}

int sb_count(void)
{
    int n = 0;
    for (int i = 0; i < MAX_ENTRIES; i++) n += s_pub[i].path[0] != 0;
    return n;
}

static void drop(struct pub *p)
{
    if (s_bus) sd_bus_emit_interfaces_removed(s_bus, p->path, IFACES[0], IFACES[1], IFACES[2], IFACES[3], NULL);
    for (int k = 0; k < 4; k++) p->slot[k] = sd_bus_slot_unref(p->slot[k]);
    p->path[0] = 0;
}

static void publish(struct pub *p, const char *path, const struct entry *e)
{
    snprintf(p->path, sizeof(p->path), "%s", path);
    snprintf(p->unit, sizeof(p->unit), "xyz.openbmc_project.Sensor.Value.Unit.%s", e->unit);
    p->value = e->value;
    p->min = e->min;
    p->max = e->max;
    if (!s_bus) return;
    int r = 0;
    for (int k = 0; r >= 0 && k < 4; k++) r = sd_bus_add_object_vtable(s_bus, &p->slot[k], p->path, IFACES[k], VTABLES[k], p);
    if (r >= 0) r = sd_bus_emit_interfaces_added(s_bus, p->path, IFACES[0], IFACES[1], IFACES[2], IFACES[3], NULL);
    if (r < 0)
    {
        fprintf(stderr, "<3>D-Bus: sensor %s: %s\n", p->path, strerror(-r));
        for (int k = 0; k < 4; k++) p->slot[k] = sd_bus_slot_unref(p->slot[k]);
        p->path[0] = 0;
    }
}

void sb_update(const struct entry *e, int n)
{
    for (int i = 0; i < MAX_ENTRIES; i++) s_pub[i].used = 0;
    for (int i = 0; i < n; i++)
    {
        if (!e[i].oname[0] || isnan(e[i].value) || !e[i].type) continue;
        char path[160];
        snprintf(path, sizeof(path), "/xyz/openbmc_project/sensors/%s/%s", e[i].type, e[i].oname);
        struct pub *p = NULL, *free_one = NULL;
        for (int k = 0; k < MAX_ENTRIES && !p; k++)
        {
            if (!strcmp(s_pub[k].path, path)) p = &s_pub[k];
            else if (!s_pub[k].path[0] && !free_one) free_one = &s_pub[k];
        }
        if (p)
        {
            p->used = 1;
            if (p->value != e[i].value)
            {
                p->value = e[i].value;
                if (s_bus) sd_bus_emit_properties_changed(s_bus, p->path, IFACES[0], "Value", NULL);
            }
            continue;
        }
        if (!free_one) continue;
        publish(free_one, path, &e[i]);
        free_one->used = free_one->path[0] != 0;
    }
    for (int i = 0; i < MAX_ENTRIES; i++)
        if (s_pub[i].path[0] && !s_pub[i].used) drop(&s_pub[i]);
}

int sb_json(struct buf *b, int first)
{
    for (int i = 0; i < MAX_ENTRIES; i++)
    {
        const struct pub *p = &s_pub[i];
        if (!p->path[0]) continue;
        if (!first) buf_str(b, ",");
        first = 0;
        buf_json(b, p->path, strlen(p->path));
        buf_str(b, ":{\"xyz.openbmc_project.Sensor.Value\":{\"Value\":");
        buf_printf(b, "%.17g", p->value);
        buf_printf(b, ",\"MaxValue\":%.17g,\"MinValue\":%.17g,\"Unit\":", p->max, p->min);
        buf_json(b, p->unit, strlen(p->unit));
        buf_str(b, "},\"xyz.openbmc_project.State.Decorator.Availability\":{\"Available\":true},"
                   "\"xyz.openbmc_project.State.Decorator.OperationalStatus\":{\"Functional\":true},"
                   "\"xyz.openbmc_project.Association.Definitions\":{\"Associations\":[[\"chassis\",\"all_sensors\",\"" SB_CHASSIS "\"]]}}");
    }
    return first;
}
