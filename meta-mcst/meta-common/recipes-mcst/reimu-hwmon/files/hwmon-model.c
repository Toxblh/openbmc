/*
 * The platform's sensors: every input of the devices in the catalog, with its
 * reading, for the web UI (its names, groups and limits, as reimu-pnsd's
 * reimu-sensors.c gave them) and for D-Bus (the object names
 * hwmon-instantiator gave phosphor-hwmon: "<device label> <input label>" with
 * [A-Za-z0-9_] only, so the IPMI SDR config and Redfish keep their paths).
 *
 * Whose reading: reimu-fand's for the chips it drives (lm96163, emc2305) and
 * the inputs of its control (the PCS, a VRM's temperature), this daemon's for
 * the rest (hwmon-chips.c). An input without a reading isn't listed, one whose
 * reading has no value is (the web UI says "no signal"), and has no D-Bus
 * object.
 *
 * Copyright (c) 2026 MCST REIMU contributors. Licensed under GPLv2.
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hwmon.h"

/* E8C2 PCS sensor places, as the host's l_pcs driver names T0..T6 */
static const char *const pcs_ru[] = {
    "ядра 0, 1", "ядра 2, 3", "ядра 4, 5", "ядра 6, 7",
    "контроллеры памяти 0, 1", "контроллеры памяти 2, 3", "системный коммутатор",
};
static const char *const pcs_en[] = { "Cores 0 1", "Cores 2 3", "Cores 4 5", "Cores 6 7",
                                      "Memory Controllers 0 1", "Memory Controllers 2 3", "System Commutator" };
#define NPCS (int)(sizeof(pcs_en) / sizeof(pcs_en[0]))

/* the LM96163 limits reimu-fand keeps (its chip guard), as the lm63 driver
 * gave them: local high 0x05, remote high 0x07, T_CRIT 0x19 */
#define LM_LOCAL_HIGH  70
#define LM_REMOTE_HIGH 105
#define LM_TCRIT       110

struct ctx
{
    const struct reading *r;
    int nr;
    struct entry *e;
    int n, max;
    char used[MAX_ENTRIES][96];
    int nused;
};

static const struct reading *find(const struct ctx *x, int bus, int addr, const char *input)
{
    char dev[12];
    snprintf(dev, sizeof(dev), "%d-%04x", bus, addr);
    for (int i = 0; i < x->nr; i++)
        if (!strcmp(x->r[i].dev, dev) && !strcmp(x->r[i].input, input)) return &x->r[i];
    return NULL;
}

/* hwmon-instantiator's object_name(): "<device> <sensor>" with [A-Za-z0-9_]
 * only, no repeated '_', unique among the sensors */
static void object_name(struct ctx *x, char *out, size_t size, const char *devlabel, const char *label)
{
    char raw[256];
    snprintf(raw, sizeof(raw), "%s %s", devlabel, label);
    size_t n = 0;
    for (const char *p = raw; *p && n + 1 < size; ++p)
    {
        int ok = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9');
        if (ok) out[n++] = *p;
        else if (n && out[n - 1] != '_') out[n++] = '_';
    }
    while (n && out[n - 1] == '_') --n;
    out[n] = '\0';
    char base[96];
    snprintf(base, sizeof(base), "%s", out);
    for (int k = 2, i = 0; i < x->nused; ++i)
        if (!strcmp(x->used[i], out))
        {
            snprintf(out, size, "%.80s_%d", base, k++);
            i = -1;
        }
    if (x->nused < MAX_ENTRIES) snprintf(x->used[x->nused++], sizeof(x->used[0]), "%s", out);
}

static const struct
{
    const char *kind, *type, *unit;
    double min, max;
} kinds[] = {
    { "temp", "temperature", "DegreesC", -128, 255 },
    { "fan", "fan_tach", "RPMS", 0, 30000 },
    { "in", "voltage", "Volts", 0, 60 },
    { "curr", "current", "Amperes", 0, 300 },
    { "power", "power", "Watts", 0, 3000 },
};

/* an entry for reading r (none: nothing) */
static struct entry *add(struct ctx *x, const struct reading *r, const char *kind, const char *group, const char *name,
                         const char *src, const char *devlabel, const char *dlabel)
{
    if (!r || x->n >= x->max) return NULL;
    struct entry *e = &x->e[x->n++];
    memset(e, 0, sizeof(*e));
    snprintf(e->kind, sizeof(e->kind), "%s", kind);
    snprintf(e->group, sizeof(e->group), "%s", group);
    snprintf(e->name, sizeof(e->name), "%s", name);
    snprintf(e->src, sizeof(e->src), "%s", src);
    e->value = r->value;
    e->warn = e->crit = NAN;
    e->duty = r->duty;
    e->at = r->at;
    for (size_t k = 0; k < sizeof(kinds) / sizeof(kinds[0]); k++)
        if (!strcmp(kinds[k].kind, kind))
        {
            e->type = kinds[k].type;
            e->unit = kinds[k].unit;
            e->min = kinds[k].min;
            e->max = kinds[k].max;
        }
    if (devlabel) object_name(x, e->oname, sizeof(e->oname), devlabel, dlabel);
    return e;
}

static void lm96163(struct ctx *x, const struct device *d, const char *src)
{
    static const char *const ru[2] = { "температура", "температура 2" };
    for (int i = 0; i < 2; i++)
    {
        const char *tl = d->tlabel[i];
        if (strstr(tl, "not connected")) continue;
        char in[8];
        snprintf(in, sizeof(in), "temp%d", i + 1);
        struct entry *e = add(x, find(x, d->bus, d->addr, in), "temp", d->label, *tl ? tl : ru[i], src, d->label, *tl ? tl : in);
        if (!e) continue;
        e->warn = i ? LM_REMOTE_HIGH : LM_LOCAL_HIGH;
        if (i) e->crit = LM_TCRIT;
    }
    add(x, find(x, d->bus, d->addr, "fan1"), "fan", d->label, "вентилятор", src, d->label, "fan1");
}

static void emc2305(struct ctx *x, const struct device *d, const char *src)
{
    for (int k = 0; k < 5; k++)
    {
        char in[8], name[48], oname[16];
        snprintf(in, sizeof(in), "fan%d", k + 1);
        snprintf(name, sizeof(name), "вентилятор %d", d->fan_base + k);
        snprintf(oname, sizeof(oname), "%d", d->fan_base + k);
        const struct reading *r = find(x, d->bus, d->addr, in);
        /* on D-Bus a fan that's there (has a reading): Fan_<the board's number> */
        add(x, r, "fan", "Вентиляторы (EMC2305)", name, src, r && !isnan(r->value) ? "Fan" : NULL, oname);
    }
}

static void pmbus(struct ctx *x, const struct device *d, const char *src)
{
    static const struct { const char *in, *kind, *ru; } rails[] = {
        { "vin", "in", "вход" }, { "vout1", "in", "выход" }, { "iout1", "curr", "ток выхода" },
        { "pout1", "power", "мощность выхода" }, { "temp1", "temp", "температура" },
    };
    for (size_t i = 0; i < sizeof(rails) / sizeof(rails[0]); i++)
    {
        const char *ru = rails[i].ru;
        if (d->kind == DK_IR3806X && !strcmp(rails[i].in, "vout1")) ru = "выход (обратная связь)";
        add(x, find(x, d->bus, d->addr, rails[i].in), rails[i].kind, d->label, ru, src, d->label, rails[i].in);
    }
}

static void pcs(struct ctx *x, const struct device *d)
{
    char group[32];
    snprintf(group, sizeof(group), "Процессор %d", d->cpu);
    for (int k = 0; k < 8; k++)
    {
        char src[16], name[96];
        snprintf(src, sizeof(src), "%d-%04x", d->bus, d->addr + k);
        if (k < NPCS) snprintf(name, sizeof(name), "%s", pcs_ru[k]);
        else snprintf(name, sizeof(name), "датчик %d", k);
        char en[32];
        snprintf(en, sizeof(en), "%s", k < NPCS ? pcs_en[k] : "Sensor 7");
        add(x, find(x, d->bus, d->addr + k, "temp1"), "temp", group, name, src, d->label, en);
    }
}

int model_build(const struct catalog *c, const struct reading *r, int nr, struct entry *e, int max)
{
    struct ctx x = { .r = r, .nr = nr, .e = e, .max = max };
    for (int i = 0; i < c->n; i++)
    {
        const struct device *d = &c->d[i];
        char src[16];
        snprintf(src, sizeof(src), "%d-%04x", d->bus, d->addr);
        switch (d->kind)
        {
        case DK_LM96163: lm96163(&x, d, src); break;
        case DK_EMC2305: emc2305(&x, d, src); break;
        case DK_PMBUS:
        case DK_IR3806X: pmbus(&x, d, src); break;
        case DK_JC42: add(&x, find(&x, d->bus, d->addr, "temp1"), "temp", d->label, "температура", src, d->label, "temp1"); break;
        case DK_PCS: pcs(&x, d); break;
        }
    }
    return x.n;
}

static const char *status_of(const struct entry *e)
{
    double v = e->value;
    return isnan(v) ? "na" : !isnan(e->crit) && v >= e->crit ? "crit" : !isnan(e->warn) && v >= e->warn ? "warn" : "ok";
}

void model_json(const struct entry *e, int n, struct buf *b)
{
    buf_str(b, "\"sensors\":[");
    for (int i = 0; i < n; i++)
    {
        const struct entry *s = &e[i];
        buf_str(b, i ? ",{\"kind\":" : "{\"kind\":");
        buf_json(b, s->kind, strlen(s->kind));
        buf_str(b, ",\"group\":");
        buf_json(b, s->group, strlen(s->group));
        buf_str(b, ",\"name\":");
        buf_json(b, s->name, strlen(s->name));
        buf_str(b, ",\"src\":");
        buf_json(b, s->src, strlen(s->src));
        if (isnan(s->value)) buf_str(b, ",\"value\":null");
        else buf_printf(b, ",\"value\":%.3f", s->value);
        if (!isnan(s->warn)) buf_printf(b, ",\"warn\":%.3f", s->warn);
        if (!isnan(s->crit)) buf_printf(b, ",\"crit\":%.3f", s->crit);
        if (!isnan(s->duty)) buf_printf(b, ",\"duty\":%.0f", s->duty);
        buf_printf(b, ",\"status\":\"%s\"}", status_of(s));
    }
    buf_str(b, "]");
}

/* a Prometheus label value: \ " and newline escaped */
static void label(struct buf *b, const char *k, const char *v, int first)
{
    buf_printf(b, "%s%s=\"", first ? "" : ",", k);
    for (const char *p = v; *p; p++)
    {
        if (*p == '\\' || *p == '"') buf_add(b, "\\", 1);
        if (*p == '\n') buf_str(b, "\\n");
        else buf_add(b, p, 1);
    }
    buf_str(b, "\"");
}

void model_metrics(const struct entry *e, int n, struct buf *b)
{
    static const struct { const char *kind, *metric, *help; } fam[] = {
        { "temp", "reimu_temperature_celsius", "Temperature of a platform sensor" },
        { "fan", "reimu_fan_rpm", "Fan speed" },
        { "in", "reimu_voltage_volts", "Voltage" },
        { "curr", "reimu_current_amperes", "Current" },
        { "power", "reimu_power_watts", "Power" },
    };
    for (size_t f = 0; f < sizeof(fam) / sizeof(fam[0]); f++)
    {
        int head = 0;
        for (int i = 0; i < n; i++)
        {
            const struct entry *s = &e[i];
            if (strcmp(s->kind, fam[f].kind) || isnan(s->value)) continue;
            if (!head++) buf_printf(b, "# HELP %s %s\n# TYPE %s gauge\n", fam[f].metric, fam[f].help, fam[f].metric);
            buf_printf(b, "%s{", fam[f].metric);
            label(b, "sensor", s->oname[0] ? s->oname : s->src, 1);
            label(b, "device", s->src, 0);
            label(b, "group", s->group, 0);
            label(b, "name", s->name, 0);
            buf_printf(b, "} %.3f\n", s->value);
        }
    }
    int head = 0;
    for (int i = 0; i < n; i++)
    {
        const struct entry *s = &e[i];
        if (strcmp(s->kind, "fan") || isnan(s->duty)) continue;
        if (!head++) buf_str(b, "# HELP reimu_fan_duty_percent Fan drive\n# TYPE reimu_fan_duty_percent gauge\n");
        buf_str(b, "reimu_fan_duty_percent{");
        label(b, "sensor", s->oname[0] ? s->oname : s->src, 1);
        label(b, "device", s->src, 0);
        label(b, "group", s->group, 0);
        label(b, "name", s->name, 0);
        buf_printf(b, "} %.1f\n", s->duty);
    }
}
