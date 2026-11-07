/* reimu-hwmon unit tests: the device trees of the board (1.0_2025-02-06, as
 * in its ПНС flash) and the next one (2.0_2026-09-15), the chips it reads
 * against register files (REIMU_HWMON_FAKE), the sensors' names, groups and
 * D-Bus paths (those of the IPMI SDR config: $IPMI_YAML), the documents.
 * Native: make test */
#define _GNU_SOURCE
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "hwmon.h"

static int s_fail, s_pass;
static char R[200];

#define CHECK(c, ...)                                     \
    do {                                                  \
        if (c) s_pass++;                                  \
        else {                                            \
            s_fail++;                                     \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                          \
            printf("\n");                                 \
        }                                                 \
    } while (0)
#define NEAR(a, b, eps) (fabs((a) - (b)) <= (eps))

static const struct device *dev(const struct catalog *c, int kind, int bus, int addr)
{
    for (int i = 0; i < c->n; i++)
        if (c->d[i].kind == kind && c->d[i].bus == bus && c->d[i].addr == addr) return &c->d[i];
    return NULL;
}

static void t_catalog(void)
{
    struct catalog c;
    CHECK(catalog_load(&c, "/nonexistent.dtb", 5) == -1, "no device tree: -1");
    CHECK(catalog_load(&c, "tests/devtree-1.0.dtb", 5) == 0, "1.0 loads");
    const struct device *d = dev(&c, DK_LM96163, 6, 0x4c);
    CHECK(d && !strcmp(d->label, "CPU FAN Control (X37)") && !strcmp(d->tlabel[0], "Internal Temp") &&
              !strcmp(d->tlabel[1], "not connected"), "1.0: the CPU fan chip 6-004c (%s)", d ? d->label : "-");
    d = dev(&c, DK_LM96163, 2, 0x4c);
    CHECK(d && !strcmp(d->tlabel[1], "IOHUB Temp"), "1.0: the board fan chip 2-004c, its remote diode the IOHUB");
    d = dev(&c, DK_EMC2305, 2, 0x2e);
    CHECK(d && d->fan_base == 0, "1.0: EMC2305 2-002e fans 0..4");
    d = dev(&c, DK_EMC2305, 2, 0x2c);
    CHECK(d && d->fan_base == 5, "1.0: EMC2305 2-002c fans 5..9");
    for (int a = 0x44; a <= 0x46; a++) CHECK(dev(&c, DK_IR3806X, 6, a), "1.0: VRM 6-%04x", a);
    d = dev(&c, DK_IR3806X, 6, 0x44);
    CHECK(d && !strcmp(d->label, "+UNCORE (in +12V)"), "1.0: its label (%s)", d ? d->label : "-");
    d = dev(&c, DK_PMBUS, 6, 0x60);
    CHECK(d && !strcmp(d->label, "Power Control"), "1.0: NCP4200 6-0060");
    CHECK(dev(&c, DK_PMBUS, 2, 0x58) && dev(&c, DK_PMBUS, 2, 0x59), "1.0: the power supplies 2-0058, 2-0059");
    d = dev(&c, DK_PCS, 5, 0x20);
    CHECK(d && d->cpu == 0 && !strcmp(d->label, "CPU0"), "1.0: PCS of CPU 0 (reg = <0 0>)");
    CHECK(c.n == 11, "1.0: 11 devices (%d)", c.n);

    CHECK(catalog_load(&c, "tests/devtree-2.0.dtb", 5) == 0, "2.0 loads");
    for (int a = 0x44; a <= 0x46; a++) CHECK(dev(&c, DK_IR3806X, 6, a), "2.0: VRM 6-%04x (\"infineon,ir38064\")", a);
    d = dev(&c, DK_IR3806X, 6, 0x44);
    CHECK(d && !strcmp(d->label, "UNCORE"), "2.0: its label (%s)", d ? d->label : "-");
    for (int a = 0x18; a <= 0x1e; a += 2) CHECK(dev(&c, DK_JC42, 6, a), "2.0: DIMM sensor 6-%04x", a);
    d = dev(&c, DK_JC42, 6, 0x18);
    CHECK(d && !strcmp(d->label, "mc0_dimm[X16]"), "2.0: its label (%s)", d ? d->label : "-");
    CHECK(!dev(&c, DK_PMBUS, 2, 0x58), "2.0: no power supplies at 0x58/0x59");
    d = dev(&c, DK_PCS, 5, 0x20);
    CHECK(d && d->cpu == 0, "2.0: PCS of CPU 0 (numa-node-id, an MMIO reg)");
    CHECK(dev(&c, DK_LM96163, 6, 0x4c) && dev(&c, DK_LM96163, 2, 0x4c) && dev(&c, DK_EMC2305, 2, 0x2c), "2.0: the fan chips");
    CHECK(c.n == 13, "2.0: 13 devices (%d)", c.n);
}

/* ------------------------------------------------------------------ fake chips */
static void mkdev(int bus, int addr)
{
    char p[512];
    snprintf(p, sizeof(p), "%s/i2c-%d", R, bus);
    mkdir(p, 0755);
    snprintf(p, sizeof(p), "%s/i2c-%d/%02x", R, bus, addr);
    uint8_t z[256] = { 0 };
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || write(fd, z, 256) != 256) abort();
    close(fd);
}

static void rmdev(int bus, int addr)
{
    char p[512];
    snprintf(p, sizeof(p), "%s/i2c-%d/%02x", R, bus, addr);
    unlink(p);
}

/* a PMBus word command (the .words file), or two registers of a plain chip */
static void setw(int bus, int addr, int reg, int w)
{
    char p[512];
    snprintf(p, sizeof(p), "%s/i2c-%d/%02x.words", R, bus, addr);
    uint8_t b[2] = { (uint8_t)(w & 0xff), (uint8_t)(w >> 8) };
    int words = !access(p, F_OK);
    if (!words) snprintf(p, sizeof(p), "%s/i2c-%d/%02x", R, bus, addr);
    int fd = open(p, O_WRONLY);
    if (fd < 0 || pwrite(fd, b, 2, words ? 2 * reg : reg) != 2) abort();
    close(fd);
}

static void pmbus_dev(int bus, int addr)
{
    mkdev(bus, addr);
    char p[512];
    snprintf(p, sizeof(p), "%s/i2c-%d/%02x.words", R, bus, addr);
    uint8_t z[512] = { 0 };
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || write(fd, z, 512) != 512) abort();
    close(fd);
}

static void setb(int bus, int addr, int reg, int v)
{
    char p[512];
    snprintf(p, sizeof(p), "%s/i2c-%d/%02x", R, bus, addr);
    uint8_t b = v;
    int fd = open(p, O_WRONLY);
    if (fd < 0 || pwrite(fd, &b, 1, reg) != 1) abort();
    close(fd);
}

/* LINEAR11 of v with exponent e */
static int lin11(double v, int e)
{
    int m = (int)lround(ldexp(v, -e)) & 0x7ff;
    return (e & 0x1f) << 11 | m;
}

static const struct reading *rd(const struct reading *r, int n, const char *dev, const char *input)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(r[i].dev, dev) && !strcmp(r[i].input, input)) return &r[i];
    return NULL;
}

static void t_chips(void)
{
    struct catalog c;
    struct reading r[MAX_READINGS];
    catalog_load(&c, "tests/devtree-2.0.dtb", 5);
    /* NCP4200: VOUT_MODE linear, 2^-9 */
    pmbus_dev(6, 0x60);
    setb(6, 0x60, 0x20, 0x17);
    setw(6, 0x60, 0x88, lin11(12.0, -4));
    setw(6, 0x60, 0x8b, (int)lround(0.9 * 512));
    setw(6, 0x60, 0x8c, lin11(20.5, -2));
    setw(6, 0x60, 0x96, lin11(18.5, -1));
    setw(6, 0x60, 0x8d, lin11(48, 0));
    /* IR38064: VOUT_MODE not answered (the fake's byte file is gone) */
    pmbus_dev(6, 0x45);
    char nb[512];
    snprintf(nb, sizeof(nb), "%s/i2c-6/45", R);
    unlink(nb);
    setw(6, 0x45, 0x88, lin11(12.125, -3));
    setw(6, 0x45, 0x8c, lin11(3.25, -2));
    setw(6, 0x45, 0x96, lin11(1.75, -2));
    setw(6, 0x45, 0x8b, 0x85);
    /* jc42: 0x05 big-endian: 0x01 0x94 = 25.25 °C; alarm bits set; and a negative one */
    mkdev(6, 0x18);
    setw(6, 0x18, 0x05, 0x94 << 8 | 0xe1);
    mkdev(6, 0x1a);
    setw(6, 0x1a, 0x05, 0xf0 << 8 | 0x1f); /* 0x1ff0: -1 °C */
    int n = 0;
    for (int i = 0; i < c.n; i++) n = chips_read(&c.d[i], r, n, MAX_READINGS, 1000);
    const struct reading *x;
    x = rd(r, n, "6-0060", "vin");
    CHECK(x && NEAR(x->value, 12.0, 0.01) && isnan(x->duty) && x->at == 1000, "NCP4200 vin %.3f", x ? x->value : -1);
    x = rd(r, n, "6-0060", "vout1");
    CHECK(x && NEAR(x->value, 0.9, 0.002), "vout1 by VOUT_MODE 2^-9: %.3f", x ? x->value : -1);
    x = rd(r, n, "6-0060", "iout1");
    CHECK(x && NEAR(x->value, 20.5, 0.01), "iout1 %.2f", x ? x->value : -1);
    x = rd(r, n, "6-0060", "pout1");
    CHECK(x && NEAR(x->value, 18.5, 0.01), "pout1 %.2f", x ? x->value : -1);
    x = rd(r, n, "6-0060", "temp1");
    CHECK(x && NEAR(x->value, 48, 0.01), "a pmbus device's temperature: ours (%.2f)", x ? x->value : -1);
    x = rd(r, n, "6-0045", "vin");
    CHECK(x && NEAR(x->value, 12.125, 0.01), "VRM vin %.3f", x ? x->value : -1);
    CHECK(!rd(r, n, "6-0045", "temp1"), "a VRM's temperature: reimu-fand's, not read here");
    x = rd(r, n, "6-0018", "temp1");
    CHECK(x && NEAR(x->value, 25.25, 0.001), "DIMM 6-0018: %.4f °C (alarm bits ignored)", x ? x->value : -1);
    x = rd(r, n, "6-001a", "temp1");
    CHECK(x && NEAR(x->value, -1, 0.001), "DIMM 6-001a: %.4f °C", x ? x->value : -1);
    CHECK(!rd(r, n, "6-001c", "temp1"), "DIMM 6-001c doesn't answer: no reading");
    CHECK(!rd(r, n, "6-004c", "temp1") && !rd(r, n, "5-0020", "temp1") && !rd(r, n, "2-002c", "fan1"),
          "the fans' chips and the PCS: never read here");
    /* one that doesn't answer is left alone for a minute */
    hw_reset();
    unsigned long x0 = hw_transfers();
    struct device *nc = NULL;
    for (int i = 0; i < c.n; i++)
        if (c.d[i].kind == DK_JC42 && c.d[i].addr == 0x1c) nc = &c.d[i];
    chips_read(nc, r, 0, MAX_READINGS, 2000);
    unsigned long t = hw_transfers() - x0;
    CHECK(t == 0, "6-001c not tried again within a minute (%lu)", t);
    /* a PMBus device that isn't there: 2 transfers, then left alone */
    struct device *ps = NULL;
    catalog_load(&c, "tests/devtree-1.0.dtb", 5);
    for (int i = 0; i < c.n; i++)
        if (c.d[i].kind == DK_PMBUS && c.d[i].addr == 0x58) ps = &c.d[i];
    x0 = hw_transfers();
    CHECK(ps && chips_read(ps, r, 0, MAX_READINGS, 3000) == 0 && hw_transfers() - x0 == 2,
          "power supply 2-0058 not there: 2 transfers (%lu)", hw_transfers() - x0);
    x0 = hw_transfers();
    chips_read(ps, r, 0, MAX_READINGS, 4000);
    CHECK(hw_transfers() == x0, "and not tried again within a minute");
    catalog_load(&c, "tests/devtree-2.0.dtb", 5);
    for (int i = 0; i < c.n; i++)
        if (c.d[i].kind == DK_JC42 && c.d[i].addr == 0x1c) nc = &c.d[i];
    mkdev(6, 0x1c);
    CHECK(chips_read(nc, r, 0, MAX_READINGS, 61000 + 1000) == 1, "tried again after it: answers now");
    rmdev(6, 0x1c);
}

/* reimu-fand's Readings as it gives them on the board */
static int fand_readings(struct reading *r, int n, int64_t at)
{
    struct { const char *dev, *in; double v, duty; } f[] = {
        { "5-0020", "temp1", 51.25, NAN }, { "5-0021", "temp1", 52, NAN }, { "5-0022", "temp1", 53, NAN },
        { "5-0023", "temp1", 54, NAN }, { "5-0024", "temp1", 55, NAN }, { "5-0025", "temp1", 56, NAN },
        { "5-0026", "temp1", 57, NAN },
        { "6-0044", "temp1", 61, NAN }, { "6-0045", "temp1", 62, NAN }, { "6-0046", "temp1", 63, NAN },
        { "2-004c", "temp2", 83, NAN }, { "6-004c", "temp1", 40, NAN }, { "6-004c", "fan1", 2000, 37 },
        { "2-004c", "temp1", 45, NAN }, { "2-004c", "fan1", 1500, 30 },
        { "2-002c", "fan1", 3000, 30 }, { "2-002c", "fan2", NAN, 30 }, { "2-002c", "fan3", NAN, 30 },
        { "2-002c", "fan4", NAN, 30 }, { "2-002c", "fan5", NAN, 30 }, { "2-002e", "fan1", NAN, 30 },
        { "2-002e", "fan2", NAN, 30 }, { "2-002e", "fan3", NAN, 30 }, { "2-002e", "fan4", NAN, 30 },
        { "2-002e", "fan5", 1200, 30 },
    };
    for (size_t i = 0; i < sizeof(f) / sizeof(f[0]); i++)
    {
        snprintf(r[n].dev, sizeof(r[n].dev), "%s", f[i].dev);
        snprintf(r[n].input, sizeof(r[n].input), "%s", f[i].in);
        r[n].value = f[i].v;
        r[n].duty = f[i].duty;
        r[n].at = at;
        n++;
    }
    return n;
}

/* a sensor published on D-Bus: a name and a value */
static const struct entry *ent(const struct entry *e, int n, const char *type, const char *oname)
{
    for (int i = 0; i < n; i++)
        if (e[i].oname[0] && !isnan(e[i].value) && !strcmp(e[i].type, type) && !strcmp(e[i].oname, oname)) return &e[i];
    return NULL;
}

static const struct entry *web(const struct entry *e, int n, const char *src, const char *name)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(e[i].src, src) && !strcmp(e[i].name, name)) return &e[i];
    return NULL;
}

/* the temperature paths of the IPMI SDR config: all of them published */
static void ipmi_paths(const struct entry *e, int n, const char *dt)
{
    const char *yaml = getenv("IPMI_YAML");
    FILE *f = yaml ? fopen(yaml, "r") : NULL;
    if (!f)
    {
        printf("  (no $IPMI_YAML: the SDR config's paths not checked)\n");
        return;
    }
    char line[512];
    int paths = 0;
    while (fgets(line, sizeof(line), f))
    {
        char *p = strstr(line, "path: /xyz/openbmc_project/sensors/");
        if (!p) continue;
        p += strlen("path: /xyz/openbmc_project/sensors/");
        p[strcspn(p, "\r\n ")] = 0;
        char *slash = strchr(p, '/');
        if (!slash) continue;
        *slash = 0;
        paths++;
        const struct entry *x = ent(e, n, p, slash + 1);
        CHECK(x && !isnan(x->value), "%s: IPMI's %s/%s published", dt, p, slash + 1);
    }
    fclose(f);
    CHECK(paths >= 10, "%s: %d sensor paths in the SDR config", dt, paths);
}

static void t_model(void)
{
    struct catalog c;
    struct reading r[MAX_READINGS];
    struct entry e[MAX_ENTRIES];
    const char *dts[] = { "tests/devtree-1.0.dtb", "tests/devtree-2.0.dtb" };
    for (int k = 0; k < 2; k++)
    {
        catalog_load(&c, dts[k], 5);
        int nr = fand_readings(r, 0, 5000);
        for (int i = 0; i < c.n; i++) nr = chips_read(&c.d[i], r, nr, MAX_READINGS, 5000);
        int n = model_build(&c, r, nr, e, MAX_ENTRIES);
        const struct entry *x;
        /* the D-Bus names hwmon-instantiator gave phosphor-hwmon */
        x = ent(e, n, "temperature", "CPU0_Cores_0_1");
        CHECK(x && NEAR(x->value, 51.25, 0.001) && !strcmp(x->unit, "DegreesC"), "%s: CPU0_Cores_0_1", dts[k]);
        x = ent(e, n, "temperature", "FAN_Control_X38_X39_X40_IOHUB_Temp");
        CHECK(x && NEAR(x->value, 83, 0.01) && x->warn == 105 && x->crit == 110, "%s: IOHUB, its limits", dts[k]);
        CHECK(ent(e, n, "temperature", "CPU_FAN_Control_X37_Internal_Temp") &&
                  ent(e, n, "temperature", "FAN_Control_X38_X39_X40_Internal_Temp"), "%s: both local temperatures", dts[k]);
        x = ent(e, n, "fan_tach", "CPU_FAN_Control_X37_fan1");
        CHECK(x && NEAR(x->value, 2000, 0.1) && NEAR(x->duty, 37, 0.1) && !strcmp(x->unit, "RPMS"), "%s: the CPU fan", dts[k]);
        CHECK(ent(e, n, "fan_tach", "FAN_Control_X38_X39_X40_fan1"), "%s: the board fan", dts[k]);
        x = ent(e, n, "fan_tach", "Fan_5");
        CHECK(x && NEAR(x->value, 3000, 0.1) && !strcmp(x->name, "вентилятор 5"), "%s: case fan 5 (2-002c fan1)", dts[k]);
        CHECK(ent(e, n, "fan_tach", "Fan_4") && !ent(e, n, "fan_tach", "Fan_6"), "%s: fan 4 there (2-002e fan5), 6 not", dts[k]);
        x = web(e, n, "2-002c", "вентилятор 6");
        CHECK(x && isnan(x->value) && NEAR(x->duty, 30, 0.1), "%s: ... in the web UI with its drive", dts[k]);
        x = ent(e, n, "voltage", "Power_Control_vin");
        CHECK(x && NEAR(x->value, 12, 0.01) && !strcmp(x->unit, "Volts"), "%s: Power_Control_vin", dts[k]);
        CHECK(ent(e, n, "voltage", "Power_Control_vout1") && ent(e, n, "current", "Power_Control_iout1") &&
                  ent(e, n, "power", "Power_Control_pout1") && ent(e, n, "temperature", "Power_Control_temp1"),
              "%s: the rest of Power Control", dts[k]);
        const char *vrm = k ? "1V2_01" : "1V2_01_in_12V";
        char nm[64];
        snprintf(nm, sizeof(nm), "%s_temp1", vrm);
        x = ent(e, n, "temperature", nm);
        CHECK(x && NEAR(x->value, 62, 0.01), "%s: %s (reimu-fand's reading)", dts[k], nm);
        snprintf(nm, sizeof(nm), "%s_vin", vrm);
        CHECK(ent(e, n, "voltage", nm) != NULL, "%s: %s", dts[k], nm);
        x = web(e, n, "6-0045", "выход (обратная связь)");
        CHECK(x && NEAR(x->value, 0x85 / 256.0, 0.001), "%s: the VRM's feedback voltage, 2^-8 (%.3f)", dts[k], x ? x->value : -1);
        if (k)
        {
            x = ent(e, n, "temperature", "mc0_dimm_X16_temp1");
            CHECK(x && NEAR(x->value, 25.25, 0.001) && !strcmp(x->group, "mc0_dimm[X16]"), "2.0: DIMM mc0");
            CHECK(!web(e, n, "6-001c", "температура"), "2.0: a DIMM sensor not answering: not listed");
        }
        else CHECK(!web(e, n, "2-0058", "вход"), "1.0: the power supplies that don't answer: not listed");
        /* reimu-fand's reading without a value (stale): no D-Bus object, "no signal" */
        r[6].value = NAN;
        struct entry e2[MAX_ENTRIES];
        int n2 = model_build(&c, r, nr, e2, MAX_ENTRIES);
        CHECK(n2 == n && !ent(e2, n2, "temperature", "CPU0_System_Commutator"), "%s: a stale one: not published", dts[k]);
        x = web(e2, n2, "5-0026", "системный коммутатор");
        CHECK(x && isnan(x->value) && !strcmp(x->group, "Процессор 0"), "%s: ... the web UI: no signal (%s %f %s)", dts[k], x ? x->src : "-", x ? x->value : -1, x ? x->group : "-");
        r[6].value = 57;
        /* names unique */
        for (int i = 0; i < n; i++)
            for (int j = i + 1; j < n; j++)
                if (e[i].oname[0] && e[j].oname[0] && !strcmp(e[i].type, e[j].type))
                    CHECK(strcmp(e[i].oname, e[j].oname), "%s: %s twice", dts[k], e[i].oname);
        ipmi_paths(e, n, dts[k]);

        /* the documents */
        struct buf b = { 0 };
        buf_str(&b, "{");
        model_json(e, n, &b);
        buf_str(&b, "}");
        char file[300], cmd[800];
        snprintf(file, sizeof(file), "%s/doc.json", R);
        FILE *fp = fopen(file, "w");
        fputs(b.s, fp);
        fclose(fp);
        snprintf(cmd, sizeof(cmd), "python3 -c 'import json,sys; d=json.load(open(sys.argv[1]))[\"sensors\"]; "
                 "s=[x for x in d if x[\"src\"]==\"2-004c\" and x[\"name\"]==\"IOHUB Temp\"][0]; "
                 "assert s[\"status\"]==\"ok\" and s[\"warn\"]==105 and s[\"group\"]==\"FAN Control (X38), (X39,X40)\", s; "
                 "assert all(x[\"status\"] in (\"ok\",\"warn\",\"crit\",\"na\") for x in d)' %s", file);
        CHECK(system(cmd) == 0, "%s: the web UI's document parses, as before", dts[k]);
        free(b.s);
        b = (struct buf){ 0 };
        model_metrics(e, n, &b);
        snprintf(file, sizeof(file), "%s/metrics.txt", R);
        fp = fopen(file, "w");
        fputs(b.s, fp);
        fclose(fp);
        snprintf(cmd, sizeof(cmd), "python3 -c 'import re,sys; t=open(sys.argv[1]).read().splitlines(); "
                 "assert all(l.startswith(\"#\") or re.match(r\"^[a-z_]+\\{([a-z]+=\\\"([^\\\"\\\\\\\\]|\\\\\\\\.)*\\\",?)+\\} -?[0-9.]+$\", l) for l in t), t; "
                 "assert any(l.startswith(\"reimu_temperature_celsius{sensor=\\\"CPU0_Cores_0_1\\\"\") for l in t)' %s", file);
        CHECK(system(cmd) == 0, "%s: Prometheus text", dts[k]);
        CHECK(strstr(b.s, "nan") == NULL && strstr(b.s, "reimu_fan_duty_percent{sensor=\"Fan_6\"") == NULL &&
                  strstr(b.s, "reimu_fan_duty_percent{sensor=\"2-002c\"") != NULL, "%s: no NaN; a fan without rpm: its drive", dts[k]);
        free(b.s);
    }
    /* reimu-fand not controlling (no Readings): only ours */
    catalog_load(&c, "tests/devtree-1.0.dtb", 5);
    int nr = 0;
    for (int i = 0; i < c.n; i++) nr = chips_read(&c.d[i], r, nr, MAX_READINGS, 9000);
    int n = model_build(&c, r, nr, e, MAX_ENTRIES);
    CHECK(!ent(e, n, "temperature", "CPU0_Cores_0_1") && !web(e, n, "6-004c", "вентилятор") && ent(e, n, "voltage", "Power_Control_vin"),
          "no Readings: no fans' or PCS sensors, ours are there");
}

int main(void)
{
    snprintf(R, sizeof(R), "%s/hwmon-test-%d", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", getpid());
    mkdir(R, 0755);
    setenv("REIMU_HWMON_FAKE", R, 1);
    struct { const char *name; void (*fn)(void); } tests[] = {
        { "catalog", t_catalog }, { "chips", t_chips }, { "model", t_model },
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++)
    {
        int f0 = s_fail;
        hw_reset();
        tests[i].fn();
        printf("%-10s %s\n", tests[i].name, s_fail == f0 ? "ok" : "FAILED");
    }
    char cmd[300];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", R);
    if (system(cmd)) return 2;
    printf("%d checks, %d failed\n", s_pass + s_fail, s_fail);
    return s_fail ? 1 : 0;
}
