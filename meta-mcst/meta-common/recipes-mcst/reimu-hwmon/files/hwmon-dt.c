/*
 * The platform's devices whose sensors reimu-hwmon knows, from the ПНС device
 * tree (/var/volatile/motherboard_devtree.dtb, put there by reimu-fru): its
 * /bmc node maps the BMC's buses onto the platform's (i2cN = "/l_i2c@0/i2c@X"
 * is BMC bus N-1); devices are known by their compatible (lm96163, emc2305,
 * pmbus, ir3806x, jc42), the E8C2 PCS by its pcs@N node. Device trees 1.0_2025
 * and 2.0 (the latter: "infineon,ir38064", jc42 DIMM sensors, pcs@0 with a
 * compatible and an MMIO reg) are what the tests use.
 *
 * Copyright (c) 2026 MCST REIMU contributors. Licensed under GPLv2.
 */
#define _GNU_SOURCE
#include <endian.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hwmon.h"

/* ------------------------------------------------------------------ */
/* flattened device tree                                               */

struct dt_prop
{
    int node;
    const char *name;
    const uint8_t *data;
    uint32_t len;
};

struct dt_node
{
    int parent;
    const char *name;
    char path[192];
};

struct dt
{
    uint8_t *blob;
    struct dt_node *nodes;
    struct dt_prop *props;
    int nnodes, nprops;
};

static void dt_free(struct dt *t)
{
    free(t->blob);
    free(t->nodes);
    free(t->props);
    memset(t, 0, sizeof(*t));
}

static int dt_load(struct dt *t, const char *file)
{
    memset(t, 0, sizeof(*t));
    FILE *f = fopen(file, "rb");
    if (!f) return -1;
    uint32_t hdr[10];
    if (fread(hdr, 4, 10, f) != 10 || be32toh(hdr[0]) != 0xd00dfeedU || be32toh(hdr[1]) > (1 << 20))
    {
        fclose(f);
        return -1;
    }
    uint32_t size = be32toh(hdr[1]), off_struct = be32toh(hdr[2]), off_strings = be32toh(hdr[3]);
    uint32_t size_strings = be32toh(hdr[8]), size_struct = be32toh(hdr[9]);
    t->blob = malloc(size + 1);
    rewind(f);
    int ok = t->blob && fread(t->blob, 1, size, f) == size;
    fclose(f);
    if (!ok || off_struct + size_struct > size || off_strings + size_strings > size)
    {
        dt_free(t);
        return -1;
    }
    t->blob[size] = 0;

    int cap_n = 64, cap_p = 256, stack[32], depth = 0;
    t->nodes = malloc(cap_n * sizeof(*t->nodes));
    t->props = malloc(cap_p * sizeof(*t->props));
    if (!t->nodes || !t->props) goto bad;
    for (uint32_t p = off_struct; p + 4 <= off_struct + size_struct;)
    {
        uint32_t tok = be32toh(*(uint32_t *)(t->blob + p));
        p += 4;
        if (tok == 1) /* BEGIN_NODE */
        {
            const char *name = (const char *)t->blob + p;
            size_t n = strnlen(name, off_struct + size_struct - p);
            p += (n + 1 + 3) & ~3U;
            if (depth == 32) goto bad;
            if (t->nnodes == cap_n)
            {
                struct dt_node *nn = realloc(t->nodes, (cap_n *= 2) * sizeof(*nn));
                if (!nn) goto bad;
                t->nodes = nn;
            }
            struct dt_node *nd = &t->nodes[t->nnodes];
            nd->parent = depth ? stack[depth - 1] : -1;
            nd->name = name;
            if (nd->parent < 0) strcpy(nd->path, "/");
            else if (snprintf(nd->path, sizeof(nd->path), "%s%s%s", t->nodes[nd->parent].path,
                              nd->parent == 0 ? "" : "/", name) >= (int)sizeof(nd->path))
                goto bad;
            stack[depth++] = t->nnodes++;
        }
        else if (tok == 2) /* END_NODE */
        {
            if (!depth--) goto bad;
        }
        else if (tok == 3) /* PROP */
        {
            if (p + 8 > off_struct + size_struct || !depth) goto bad;
            uint32_t len = be32toh(*(uint32_t *)(t->blob + p)), nameoff = be32toh(*(uint32_t *)(t->blob + p + 4));
            p += 8;
            if (p + len > off_struct + size_struct || nameoff >= size_strings) goto bad;
            if (t->nprops == cap_p)
            {
                struct dt_prop *np = realloc(t->props, (cap_p *= 2) * sizeof(*np));
                if (!np) goto bad;
                t->props = np;
            }
            t->props[t->nprops++] = (struct dt_prop){ stack[depth - 1], (const char *)t->blob + off_strings + nameoff,
                                                      t->blob + p, len };
            p += (len + 3) & ~3U;
        }
        else if (tok == 4) continue; /* NOP */
        else break;                  /* END */
    }
    return 0;
bad:
    dt_free(t);
    return -1;
}

static int dt_find(const struct dt *t, const char *path)
{
    for (int i = 0; i < t->nnodes; i++)
        if (!strcmp(t->nodes[i].path, path)) return i;
    return -1;
}

static const struct dt_prop *dt_prop(const struct dt *t, int node, const char *name)
{
    for (int i = 0; i < t->nprops; i++)
        if (t->props[i].node == node && !strcmp(t->props[i].name, name)) return &t->props[i];
    return NULL;
}

/* string property, NULL unless NUL-terminated */
static const char *dt_str(const struct dt *t, int node, const char *name)
{
    const struct dt_prop *p = dt_prop(t, node, name);
    return p && p->len && !p->data[p->len - 1] ? (const char *)p->data : NULL;
}

static int dt_u32(const struct dt *t, int node, const char *name, uint32_t *v)
{
    const struct dt_prop *p = dt_prop(t, node, name);
    if (!p || p->len < 4) return -1;
    *v = be32toh(*(const uint32_t *)p->data);
    return 0;
}

/* ------------------------------------------------------------------ */
/* the catalog                                                          */

static int kind_of(const char *compat)
{
    if (strstr(compat, "lm96163") || strstr(compat, "lm63")) return DK_LM96163;
    if (strstr(compat, "emc2305")) return DK_EMC2305;
    if (strstr(compat, "ir3806")) return DK_IR3806X;
    if (!strcmp(compat, "pmbus")) return DK_PMBUS;
    if (strstr(compat, "jc-42") || strstr(compat, "jc42")) return DK_JC42;
    return -1;
}

static void device(struct catalog *c, const struct dt *t, int node, int bus)
{
    const char *compat = dt_str(t, node, "compatible");
    const char *status = dt_str(t, node, "status");
    uint32_t addr;
    if (!compat || dt_u32(t, node, "reg", &addr) || addr > 0x7f) return;
    if (status && strcmp(status, "okay") && strcmp(status, "ok")) return;
    int kind = kind_of(compat);
    if (kind < 0 || c->n >= MAX_DEVS) return;
    struct device *d = &c->d[c->n++];
    memset(d, 0, sizeof(*d));
    d->kind = kind;
    d->bus = bus;
    d->addr = addr;
    const char *label = dt_str(t, node, "label");
    snprintf(d->label, sizeof(d->label), "%s", label ? label : t->nodes[node].name);
    for (int i = 0; i < 2; i++)
    {
        char a[16];
        snprintf(a, sizeof(a), "temp%d_label", i + 1);
        const char *l = dt_str(t, node, a);
        snprintf(d->tlabel[i], sizeof(d->tlabel[i]), "%s", l ? l : "");
    }
    /* "FAN: PWM[0..4] TACH[0..4]": the board's numbers of its fans */
    const char *p = label ? strstr(label, "TACH[") : NULL;
    d->fan_base = p ? atoi(p + 5) : 0;
}

int catalog_load(struct catalog *c, const char *dtb, int pcs_bus)
{
    struct dt t;
    memset(c, 0, sizeof(*c));
    if (dt_load(&t, dtb)) return -1;
    int bmc = dt_find(&t, "/bmc");
    for (int i = 0; bmc >= 0 && i < t.nprops; i++)
    {
        const struct dt_prop *p = &t.props[i];
        if (p->node != bmc || strncmp(p->name, "i2c", 3) || !p->len || p->data[p->len - 1]) continue;
        char *end;
        long n = strtol(p->name + 3, &end, 10);
        int target = dt_find(&t, (const char *)p->data);
        if (*end || n < 1 || target < 0) continue;
        for (int k = 0; k < t.nnodes; k++)
            if (t.nodes[k].parent == target) device(c, &t, k, (int)n - 1);
    }
    for (int k = 0; k < t.nnodes && c->n < MAX_DEVS; k++)
    {
        uint32_t reg;
        if (t.nodes[k].parent != 0 || strncmp(t.nodes[k].name, "pcs@", 4)) continue;
        /* 1.0: reg = <node 0>, no numa-node-id; 2.0: numa-node-id, an MMIO reg */
        int cpu = dt_u32(&t, k, "numa-node-id", &reg) == 0 ? (int)reg : atoi(t.nodes[k].name + 4);
        if (cpu < 0 || cpu > 3) continue;
        struct device *d = &c->d[c->n++];
        memset(d, 0, sizeof(*d));
        d->kind = DK_PCS;
        d->bus = pcs_bus;
        d->addr = 0x20 + 0x10 * cpu;
        d->cpu = cpu;
        snprintf(d->label, sizeof(d->label), "CPU%d", cpu);
    }
    dt_free(&t);
    return 0;
}
