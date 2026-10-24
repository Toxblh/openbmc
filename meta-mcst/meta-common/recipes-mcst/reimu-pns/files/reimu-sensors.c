/*
 * Platform sensors for the web UI, read by reimu-pnsd.
 *
 * The ПНС device tree (/var/volatile/motherboard_devtree.dtb, put there by
 * mcst-fruid) lists the platform's I2C devices; its /bmc node maps the
 * BMC's buses onto them (i2cN = "/l_i2c@0/i2c@X" is BMC bus N-1). Current
 * device trees (1.0_2025, 2.0) describe a device with properties such as
 * temp1_label, not with the per-sensor child nodes hwmon-instantiator
 * expects, so phosphor-hwmon gets empty configs and Redfish shows nothing.
 *
 * A device is read through its kernel driver when one is bound (its hwmon
 * directory), otherwise directly over i2c-dev:
 *   lm96163 (lm63)  temps from hwmon; tachometer from registers (the driver
 *                   exposes no fan when it probed before the ПНС set the pin)
 *   pmbus, ir38064  hwmon or PMBus READ_VIN/VOUT/IOUT/TEMP/POUT (linear format)
 *   emc2305         registers: fan drive and tachometer (no driver on BMC)
 *   pcs@N           E8C2 Power Control System: CPU N temperatures, 2-byte reads
 *                   at 0x20+0x10*N+k on BMC bus REIMU_PCS_BUS (default 5)
 *
 * Only reads, and only while the platform runs: its sensors are powered
 * with it, and the bus must be left alone while it powers up.
 *
 * Copyright (c) 2026 MCST REIMU contributors. Licensed under GPLv2.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "reimu-sensors.h"

#define DTB_FILE "/var/volatile/motherboard_devtree.dtb"
#define PCS_BUS  5

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
/* output                                                              */

struct out
{
    struct buf *b;
    int n;
};

static void json_str(struct buf *b, const char *s)
{
    buf_json(b, s, strlen(s));
}

/* kind: temp fan in curr power; value NAN = no reading.
 * warn/crit: upper limits, NAN = none. duty: fan drive %, <0 = none */
static void emit(struct out *o, const char *kind, const char *group, const char *name, const char *src, double value,
                 double warn, double crit, double duty)
{
    struct buf *b = o->b;
    buf_str(b, o->n++ ? ",{" : "{");
    buf_str(b, "\"kind\":");
    json_str(b, kind);
    buf_str(b, ",\"group\":");
    json_str(b, group);
    buf_str(b, ",\"name\":");
    json_str(b, name);
    buf_str(b, ",\"src\":");
    json_str(b, src);
    if (isnan(value)) buf_str(b, ",\"value\":null");
    else buf_printf(b, ",\"value\":%.3f", value);
    if (!isnan(warn)) buf_printf(b, ",\"warn\":%.3f", warn);
    if (!isnan(crit)) buf_printf(b, ",\"crit\":%.3f", crit);
    if (duty >= 0) buf_printf(b, ",\"duty\":%.0f", duty);
    const char *st = isnan(value) ? "na" : !isnan(crit) && value >= crit ? "crit" : !isnan(warn) && value >= warn ? "warn" : "ok";
    buf_printf(b, ",\"status\":\"%s\"}", st);
}

/* ------------------------------------------------------------------ */
/* hwmon and i2c-dev access                                            */

static int hwmon_dir(int bus, int addr, char *out, size_t n)
{
    char d[96];
    snprintf(d, sizeof(d), "/sys/bus/i2c/devices/%d-%04x/hwmon", bus, addr);
    DIR *dir = opendir(d);
    if (!dir) return -1;
    struct dirent *e;
    int rv = -1;
    while ((e = readdir(dir)))
        if (!strncmp(e->d_name, "hwmon", 5))
        {
            rv = snprintf(out, n, "%s/%s", d, e->d_name) < (int)n ? 0 : -1;
            break;
        }
    closedir(dir);
    return rv;
}

static int read_long(const char *dir, const char *attr, long *v)
{
    char p[192], s[32];
    snprintf(p, sizeof(p), "%s/%s", dir, attr);
    int fd = open(p, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, s, sizeof(s) - 1);
    close(fd);
    if (n <= 0) return -1;
    s[n] = 0;
    char *end;
    *v = strtol(s, &end, 10);
    return end == s ? -1 : 0;
}

static int read_text(const char *dir, const char *attr, char *s, size_t n)
{
    char p[192];
    snprintf(p, sizeof(p), "%s/%s", dir, attr);
    int fd = open(p, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t r = read(fd, s, n - 1);
    close(fd);
    if (r <= 0) return -1;
    s[r] = 0;
    s[strcspn(s, "\n")] = 0;
    return 0;
}

static double attr_or_nan(const char *dir, const char *attr, double scale)
{
    long v;
    return read_long(dir, attr, &v) ? NAN : v / scale;
}

static int i2c_open(int bus, int addr)
{
    char p[32];
    snprintf(p, sizeof(p), "/dev/i2c-%d", bus);
    int fd = open(p, O_RDWR);
    if (fd < 0) return -1;
    /* FORCE: the address may be claimed by a driver (lm63) or by a device
     * without a driver; these are register reads only */
    if (ioctl(fd, I2C_SLAVE_FORCE, addr) < 0)
    {
        close(fd);
        return -1;
    }
    return fd;
}

static int smbus(int fd, char rw, uint8_t cmd, int size, union i2c_smbus_data *d)
{
    struct i2c_smbus_ioctl_data a = { rw, cmd, size, d };
    return ioctl(fd, I2C_SMBUS, &a);
}

static int rd_byte(int fd, uint8_t cmd)
{
    union i2c_smbus_data d;
    return smbus(fd, I2C_SMBUS_READ, cmd, I2C_SMBUS_BYTE_DATA, &d) < 0 ? -1 : d.byte;
}

static int rd_word(int fd, uint8_t cmd)
{
    union i2c_smbus_data d;
    return smbus(fd, I2C_SMBUS_READ, cmd, I2C_SMBUS_WORD_DATA, &d) < 0 ? -1 : d.word;
}

/* PMBus LINEAR11: 5-bit exponent, 11-bit mantissa, both signed */
static double linear11(int w)
{
    int e = (w >> 11) & 0x1f, m = w & 0x7ff;
    if (e & 0x10) e -= 32;
    if (m & 0x400) m -= 2048;
    return ldexp(m, e);
}

/* ------------------------------------------------------------------ */
/* devices                                                             */

static const char *pmbus_name(const char *label, char *tmp, size_t n)
{
    static const struct { const char *k, *v; } names[] = {
        { "vin", "вход" }, { "vout1", "выход" }, { "iin", "ток входа" }, { "iout1", "ток выхода" },
        { "pin", "мощность входа" }, { "pout1", "мощность выхода" },
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (!strcmp(label, names[i].k)) return names[i].v;
    snprintf(tmp, n, "%s", label);
    return tmp;
}

/* every temp/in/curr/power/fan input of a hwmon device. dt labels
 * (temp1_label = "...") win over the driver's; "not connected" is skipped */
static void read_hwmon(struct out *o, const struct dt *t, int node, const char *group, const char *src, const char *dir)
{
    static const struct { const char *type, *kind, *ru; double scale; } types[] = {
        { "temp", "temp", "температура", 1000 }, { "in", "in", "напряжение", 1000 }, { "curr", "curr", "ток", 1000 },
        { "power", "power", "мощность", 1e6 }, { "fan", "fan", "вентилятор", 1 },
    };
    for (size_t ti = 0; ti < sizeof(types) / sizeof(types[0]); ti++)
        for (int i = 0; i <= 12; i++)
        {
            char a[48], label[96], tmp[96];
            long v;
            snprintf(a, sizeof(a), "%s%d_input", types[ti].type, i);
            if (read_long(dir, a, &v)) continue;
            snprintf(a, sizeof(a), "%s%d_label", types[ti].type, i);
            const char *dl = dt_str(t, node, a);
            if (dl && strstr(dl, "not connected")) continue;
            const char *name;
            if (dl) name = dl;
            else if (!read_text(dir, a, label, sizeof(label))) name = pmbus_name(label, tmp, sizeof(tmp));
            else
            {
                snprintf(tmp, sizeof(tmp), i > 1 ? "%s %d" : "%s", types[ti].ru, i);
                name = tmp;
            }
            double sc = types[ti].scale, warn, crit;
            snprintf(a, sizeof(a), "%s%d_max", types[ti].type, i);
            warn = attr_or_nan(dir, a, sc);
            snprintf(a, sizeof(a), "%s%d_crit", types[ti].type, i);
            crit = attr_or_nan(dir, a, sc);
            /* lm96163 temp1_max is the fan control's limit, pmbus in/power
             * maxima are full scale: only temperatures get thresholds */
            if (strcmp(types[ti].kind, "temp")) warn = crit = NAN;
            emit(o, types[ti].kind, group, name, src, v / sc, warn, crit, -1);
        }
}

/* LM96163 tachometer: TACH count LSB 0x46 (read first: latches MSB), MSB 0x47,
 * enabled by CONFIG (0x03) bit 2; RPM = 5400000 / count, as the lm63 driver */
static void lm96163_fan(struct out *o, int bus, int addr, const char *group, const char *src, const char *hwdir)
{
    long pwm = -1;
    if (hwdir) read_long(hwdir, "pwm1", &pwm);
    int fd = i2c_open(bus, addr);
    if (fd < 0) return;
    int cfg = rd_byte(fd, 0x03), lo = cfg >= 0 && (cfg & 0x04) ? rd_byte(fd, 0x46) : -1, hi = lo >= 0 ? rd_byte(fd, 0x47) : -1;
    close(fd);
    if (hi < 0) return;
    int count = ((hi << 8) | lo) & 0xfffc;
    double rpm = count == 0 || count == 0xfffc ? NAN : 5400000.0 / count;
    emit(o, "fan", group, "вентилятор", src, rpm, NAN, NAN, pwm >= 0 ? pwm * 100.0 / 255 : -1);
}

/* PMBus device without a driver. VOUT_MODE (0x20) gives the LINEAR16
 * exponent; IR38064 does not answer it and uses 2^-8 (its POUT, computed
 * by the chip, agrees). That is the voltage at the chip's feedback pin:
 * on 1Э8СВ the "+1V2" rails read ~0.52 V (VOUT_COMMAND 0x80 = 0.5 V, no
 * VOUT_SCALE_LOOP), so the rail itself is set by an external divider */
static void pmbus_direct(struct out *o, int bus, int addr, const char *compat, const char *group, const char *src)
{
    int fd = i2c_open(bus, addr);
    if (fd < 0) return;
    int w, mode = rd_byte(fd, 0x20), exp = -8;
    int vout_ok = 1;
    if (mode >= 0)
    {
        if ((mode >> 5) == 0) exp = (mode & 0x10) ? (mode & 0x1f) - 32 : (mode & 0x1f);
        else vout_ok = 0; /* VID/direct: needs the device's tables */
    }
    else if (!strstr(compat, "ir3806")) vout_ok = 0;
    if ((w = rd_word(fd, 0x88)) >= 0) emit(o, "in", group, "вход", src, linear11(w), NAN, NAN, -1);
    if (vout_ok && (w = rd_word(fd, 0x8b)) >= 0)
        emit(o, "in", group, mode >= 0 ? "выход" : "выход (обратная связь)", src, ldexp(w, exp), NAN, NAN, -1);
    if ((w = rd_word(fd, 0x8c)) >= 0) emit(o, "curr", group, "ток выхода", src, linear11(w), NAN, NAN, -1);
    if ((w = rd_word(fd, 0x96)) >= 0) emit(o, "power", group, "мощность выхода", src, linear11(w), NAN, NAN, -1);
    if ((w = rd_word(fd, 0x8d)) >= 0) emit(o, "temp", group, "температура", src, linear11(w), NAN, NAN, -1);
    close(fd);
}

/* EMC2305: fan n (0..4) registers at 0x30+0x10*n: drive +0, CONFIG1 +2
 * (RANGE bits 6:5 -> m = 1,2,4,8), TACH reading +0x0e/+0x0f (13 bits).
 * RPM = 3932160 * m / count (2 poles, 5 edges), as the Linux driver. The
 * dt label "FAN: PWM[0..4] TACH[0..4]" gives the board's fan numbers */
static void emc2305(struct out *o, int bus, int addr, const char *label, const char *src)
{
    int fd = i2c_open(bus, addr);
    if (fd < 0) return;
    if (rd_byte(fd, 0xfd) != 0x34 || rd_byte(fd, 0xfe) != 0x5d)
    {
        close(fd);
        return;
    }
    const char *p = label ? strstr(label, "TACH[") : NULL;
    int base = p ? atoi(p + 5) : 0;
    for (int n = 0; n < 5; n++)
    {
        int r = 0x30 + 0x10 * n;
        int drive = rd_byte(fd, r), cfg = rd_byte(fd, r + 2), hi = rd_byte(fd, r + 0x0e), lo = rd_byte(fd, r + 0x0f);
        if (drive < 0 || cfg < 0 || hi < 0 || lo < 0) continue;
        int count = ((hi << 8) | lo) >> 3, m = 1 << ((cfg >> 5) & 3);
        /* a stopped or absent fan reads as the 13-bit maximum */
        double rpm = count == 0 || count >= 0x1ff8 ? NAN : 3932160.0 * m / count;
        char name[48];
        snprintf(name, sizeof(name), "вентилятор %d", base + n);
        emit(o, "fan", "Вентиляторы (EMC2305)", name, src, rpm, NAN, NAN, drive * 100.0 / 255);
    }
    close(fd);
}

/* E8C2 PCS: 2-byte big-endian reading in 1/8 °C, 0 = no sensor. Sensor
 * places as the host's l_pcs driver names T0..T6. */
static const char *const pcs_names[] = {
    "ядра 0, 1", "ядра 2, 3", "ядра 4, 5", "ядра 6, 7",
    "контроллеры памяти 0, 1", "контроллеры памяти 2, 3", "системный коммутатор",
};

static void pcs(struct out *o, int bus, int node)
{
    char group[32];
    snprintf(group, sizeof(group), "Процессор %d", node);
    for (int k = 0; k < 8; k++)
    {
        int addr = 0x20 + 0x10 * node + k;
        unsigned v;
        char hw[128];
        long mc;
        if (!hwmon_dir(bus, addr, hw, sizeof(hw)))
        {
            /* l_pcs_i2c (hwmon-instantiator): its reading, not a second one */
            if (read_long(hw, "temp1_input", &mc))
            {
                if (k == 0) return;
                continue;
            }
            v = mc / 125;
        }
        else
        {
            int fd = i2c_open(bus, addr);
            if (fd < 0) return;
            uint8_t b[2];
            ssize_t n = read(fd, b, 2);
            close(fd);
            if (n != 2)
            {
                if (k == 0) return; /* no such CPU */
                continue;
            }
            v = (b[0] << 8) | b[1];
        }
        if (!v) continue;
        char name[64], src[24];
        if (k < (int)(sizeof(pcs_names) / sizeof(pcs_names[0]))) snprintf(name, sizeof(name), "%s", pcs_names[k]);
        else snprintf(name, sizeof(name), "датчик %d", k);
        snprintf(src, sizeof(src), "%d-%04x", bus, addr);
        emit(o, "temp", group, name, src, v / 8.0, NAN, NAN, -1);
    }
}

static void device(struct out *o, const struct dt *t, int node, int bus)
{
    const char *compat = dt_str(t, node, "compatible");
    const char *status = dt_str(t, node, "status");
    uint32_t addr;
    if (!compat || dt_u32(t, node, "reg", &addr) || addr > 0x7f) return;
    if (status && strcmp(status, "okay") && strcmp(status, "ok")) return;
    const char *label = dt_str(t, node, "label");
    char src[24], group[128], hw[128];
    snprintf(src, sizeof(src), "%d-%04x", bus, addr);
    snprintf(group, sizeof(group), "%s", label ? label : t->nodes[node].name);
    int has_hw = !hwmon_dir(bus, addr, hw, sizeof(hw));

    if (strstr(compat, "lm96163") || strstr(compat, "lm63"))
    {
        if (has_hw) read_hwmon(o, t, node, group, src, hw);
        long v;
        if (!has_hw || read_long(hw, "fan1_input", &v)) lm96163_fan(o, bus, addr, group, src, has_hw ? hw : NULL);
    }
    else if (strstr(compat, "emc2305"))
    {
        if (has_hw) read_hwmon(o, t, node, group, src, hw);
        else emc2305(o, bus, addr, label, src);
    }
    else if (!strcmp(compat, "pmbus") || strstr(compat, "ir3806"))
    {
        if (has_hw) read_hwmon(o, t, node, group, src, hw);
        else pmbus_direct(o, bus, addr, compat, group, src);
    }
    else if (has_hw && !strstr(compat, "24c") && !strstr(compat, "spd") && !strstr(compat, "mcp79"))
        read_hwmon(o, t, node, group, src, hw); /* e.g. jc42 in devtree 2.0, with a driver */
}

void sensors_json(struct buf *b)
{
    struct dt t;
    struct out o = { b, 0 };
    buf_str(b, "\"sensors\":[");
    const char *file = getenv("REIMU_DTB");
    if (dt_load(&t, file ? file : DTB_FILE))
    {
        buf_str(b, "],\"error\":\"no device tree\"");
        return;
    }
    int bmc = dt_find(&t, "/bmc");
    for (int i = 0; bmc >= 0 && i < t.nprops; i++)
    {
        const struct dt_prop *p = &t.props[i];
        if (p->node != bmc || strncmp(p->name, "i2c", 3) || !p->len || p->data[p->len - 1]) continue;
        char *end;
        long n = strtol(p->name + 3, &end, 10);
        int target = dt_find(&t, (const char *)p->data);
        if (*end || n < 1 || target < 0) continue;
        for (int c = 0; c < t.nnodes; c++)
            if (t.nodes[c].parent == target) device(&o, &t, c, (int)n - 1);
    }
    const char *pb = getenv("REIMU_PCS_BUS");
    int pcs_bus = pb ? atoi(pb) : PCS_BUS;
    for (int c = 0; c < t.nnodes; c++)
    {
        uint32_t reg;
        if (t.nodes[c].parent != 0 || strncmp(t.nodes[c].name, "pcs@", 4)) continue;
        /* 1.0: reg = <node 0>; 2.0: pcs@<node>,<offset> */
        int node = dt_u32(&t, c, "numa-node-id", &reg) == 0 ? (int)reg : atoi(t.nodes[c].name + 4);
        pcs(&o, pcs_bus, node);
    }
    buf_str(b, "]");
    dt_free(&t);
}
