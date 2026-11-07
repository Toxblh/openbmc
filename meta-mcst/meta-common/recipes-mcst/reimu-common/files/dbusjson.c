/*
 * D-Bus replies as JSON, for the web UI's view (reimu-pnsd's view/static,
 * reimu-hwmon's view/live): see dbusjson.h. Every call has a timeout
 * (DJ_CALL_USEC): one that hangs leaves its part out, not the document.
 *
 * Copyright (c) 2026 MCST REIMU contributors. Licensed under GPLv2.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <systemd/sd-bus.h>

#include "dbusjson.h"
#include "rbuf.h"

/* a D-Bus message (its current position) as JSON */
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
int dj_value(sd_bus_message *m, struct buf *b)
{
    char type;
    const char *contents;
    int r = sd_bus_message_peek_type(m, &type, &contents);
    if (r <= 0) return r;
    if (type == SD_BUS_TYPE_VARIANT || type == SD_BUS_TYPE_STRUCT)
    {
        r = sd_bus_message_enter_container(m, type, contents);
        if (r <= 0) return r;
        if (type == SD_BUS_TYPE_VARIANT) r = dj_value(m, b);
        else
        {
            buf_str(b, "[");
            int first = 1;
            while ((r = sd_bus_message_at_end(m, 0)) == 0)
            {
                if (!first) buf_str(b, ",");
                first = 0;
                if ((r = dj_value(m, b)) < 0) return r;
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
                if ((r = dj_value(m, b)) < 0) return r;
                if (sd_bus_message_exit_container(m) < 0) return -1;
            }
            else if ((r = dj_value(m, b)) < 0) return r;
        }
        buf_str(b, dict ? "}" : "]");
        return sd_bus_message_exit_container(m) < 0 ? -1 : 1;
    }
    return basic_json(m, type, b);
}

/* a method call without arguments, with a timeout; the reply or NULL */
sd_bus_message *dj_call(sd_bus *bus, const char *dest, const char *path, const char *iface, const char *member)
{
    sd_bus_message *m = NULL, *reply = NULL;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    if (sd_bus_message_new_method_call(bus, &m, dest, path, iface, member) >= 0)
        sd_bus_call(bus, m, DJ_CALL_USEC, &err, &reply);
    sd_bus_error_free(&err);
    sd_bus_message_unref(m);
    return reply;
}

/* ObjectManager.GetManagedObjects of a service: path -> interface -> properties,
 * only the paths under PREFIX; appended without the braces (for merging) */
int dj_managed(sd_bus *bus, struct buf *b, const char *dest, const char *om, const char *prefix, int first)
{
    sd_bus_message *r = dj_call(bus, dest, om, "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
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
                if (dj_value(r, b) <= 0)
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

/* an object of another service: all the properties of its interface (what
 * bmcweb's REST gives), {} if it can't be read */
void dj_object(sd_bus *bus, struct buf *b, const char *dest, const char *path, const char *iface)
{
    buf_json(b, path, strlen(path));
    buf_str(b, ":");
    sd_bus_message *m = NULL, *r = NULL;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    if (sd_bus_message_new_method_call(bus, &m, dest, path, "org.freedesktop.DBus.Properties", "GetAll") >= 0 &&
        sd_bus_message_append(m, "s", iface) >= 0)
        sd_bus_call(bus, m, DJ_CALL_USEC, &err, &r);
    sd_bus_error_free(&err);
    sd_bus_message_unref(m);
    size_t at = b->len;
    if (!r || dj_value(r, b) <= 0)
    {
        b->len = at;
        if (b->s) b->s[at] = 0;
        buf_str(b, "{}");
    }
    sd_bus_message_unref(r);
}
