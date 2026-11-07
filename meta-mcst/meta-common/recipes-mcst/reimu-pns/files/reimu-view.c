/*
 * The web UI's view of the BMC, its static part: what changes at a boot or a
 * flash write, in one answer (its live part is reimu-hwmon's view/live, which
 * carries this one's version).
 *
 *   object    /xyz/openbmc_project/reimu/view/static
 *   interface xyz.openbmc_project.Reimu.View
 *     property Json (s): hash (the version), objects (the ПНС document),
 *       inventory (path -> interface -> properties), bmc (FirmwareVersion,
 *       UUID as bmcweb makes it)
 *     property Hash (s): the version alone (view/live gives it to the pages:
 *       they fetch Json again only when it changes)
 *
 * Read on demand: nothing polled in the background. The inventory call has a
 * timeout (dbusjson.c).
 *
 * Copyright (c) 2026 MCST REIMU contributors. Licensed under GPLv2.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <systemd/sd-bus.h>
#include <systemd/sd-id128.h>

#include "dbusjson.h"
#include "rbuf.h"
#include "reimu-view.h"

static sd_bus *bus;

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
    dj_managed(bus, &body, "xyz.openbmc_project.Inventory.Manager", "/xyz/openbmc_project/inventory",
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

/* ------------------------------------------------------------------ */
/* D-Bus                                                               */

static int prop_static(sd_bus *bs, const char *path, const char *iface, const char *prop, sd_bus_message *reply,
                       void *ud, sd_bus_error *err)
{
    (void)bs; (void)path; (void)iface; (void)ud; (void)err;
    build_static();
    if (!strcmp(prop, "Hash"))
    {
        char h[20];
        snprintf(h, sizeof(h), "%016llx", (unsigned long long)static_hash);
        return sd_bus_message_append(reply, "s", h);
    }
    return sd_bus_message_append(reply, "s", static_doc ? static_doc : "{}");
}

static const sd_bus_vtable static_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("Json", "s", prop_static, 0, SD_BUS_VTABLE_PROPERTY_EMITS_INVALIDATION),
    SD_BUS_PROPERTY("Hash", "s", prop_static, 0, SD_BUS_VTABLE_PROPERTY_EMITS_INVALIDATION),
    SD_BUS_VTABLE_END
};

int view_register(sd_bus *b)
{
    bus = b;
    return sd_bus_add_object_vtable(b, NULL, "/xyz/openbmc_project/reimu/view/static", "xyz.openbmc_project.Reimu.View",
                                    static_vtable, NULL);
}

/* reimu-pnsd --view static: the document, for tests and a look by hand */
int view_print(sd_bus *b, const char *which)
{
    bus = b;
    if (strcmp(which, "static")) return 1;
    build_static();
    if (static_doc) puts(static_doc);
    return static_doc ? 0 : 1;
}
