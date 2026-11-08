/* reimu-fand unit tests: the control logic against fake chips (register
 * files, REIMU_FAND_FAKE), with simulated time. Native: make test */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "fand.h"

/* the transfers of fand.c (-Wl,--wrap, see the Makefile): faults put in, and the
 * time they take, simulated: a transfer on a dead bus takes its ~1.5 s */
int __real_hw_rd_byte(int bus, int addr, int reg);
int __real_hw_wr_byte(int bus, int addr, int reg, int val);
static struct fault {
    int bus, addr, reg;  /* writes to it ...: */
    int lose;            /* ... this many are lost, acknowledged */
    int nak;             /* ... this many are refused */
    int stick;           /* these bits of 0x4A can't be cleared */
    int lock;            /* 0x4A takes no write at all, acknowledged */
    int rd_fail_from;    /* reads of it fail after this many, -1: never */
    int dead_bus;        /* every transfer: fails, in 1.5 s; -2: all of them */
    int dead_wr_bus;     /* every write: the same */
    int hang_rest;       /* every transfer but the ID registers and the first reads of 0x4A: the same */
} I;
static int64_t s_hw_ms;
int64_t __wrap_hw_now_ms(void) { return s_hw_ms; }
/* what went over each bus, and the EMC2305 drive writes (the watchdog's food) */
static unsigned long s_bus_xfers[16], s_emc_drive_wr, s_emc_tach_rd;
static int64_t s_emc_last_wr[2], s_emc_max_gap;
static int64_t T; /* simulated ms */
static void no_faults(void)
{
    memset(&I, 0, sizeof(I));
    I.bus = I.dead_bus = I.dead_wr_bus = -1;
    I.rd_fail_from = -1;
}
int __wrap_hw_rd_byte(int bus, int addr, int reg)
{
    s_hw_ms += 1;
    s_bus_xfers[bus & 15]++;
    if (bus == 2 && addr == 0x2c && reg == 0x3e) s_emc_tach_rd++;
    if (I.dead_bus == -2 || bus == I.dead_bus || (I.hang_rest && reg < 0xfd && reg != 0x4a))
    {
        s_hw_ms += 1500;
        return -1;
    }
    if (bus == I.bus && addr == I.addr && reg == I.reg && I.rd_fail_from >= 0 && I.rd_fail_from-- == 0)
    {
        I.rd_fail_from = 0;
        return -1;
    }
    return __real_hw_rd_byte(bus, addr, reg);
}
int __wrap_hw_wr_byte(int bus, int addr, int reg, int val)
{
    s_hw_ms += 1;
    s_bus_xfers[bus & 15]++;
    if (bus == 2 && (addr == 0x2c || addr == 0x2e) && reg >= 0x30 && reg <= 0x70 && !(reg & 0x0f))
    {
        int k = addr == 0x2e;
        s_emc_drive_wr++;
        if (s_emc_last_wr[k] && T - s_emc_last_wr[k] > s_emc_max_gap) s_emc_max_gap = T - s_emc_last_wr[k];
        s_emc_last_wr[k] = T;
    }
    if (I.dead_bus == -2 || bus == I.dead_bus || bus == I.dead_wr_bus || I.hang_rest)
    {
        s_hw_ms += 1500;
        return -1;
    }
    if (bus == I.bus && addr == I.addr && reg == I.reg)
    {
        if (I.nak > 0)
        {
            I.nak--;
            return -1;
        }
        if (I.lose > 0)
        {
            I.lose--;
            return 0;
        }
        if (I.lock) return 0;
        if (reg == 0x4a) val |= I.stick;
    }
    return __real_hw_wr_byte(bus, addr, reg, val);
}

static int s_fail, s_pass;
static char R[200]; /* the fake root */

#define CHECK(c, ...)                                                   \
    do {                                                                \
        if (c) s_pass++;                                                \
        else {                                                          \
            s_fail++;                                                   \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);               \
            printf(__VA_ARGS__);                                        \
            printf("\n");                                               \
        }                                                               \
    } while (0)
#define NEAR(a, b, eps) (fabs((a) - (b)) <= (eps))

/* ------------------------------------------------------------------ fake chips */
static void path(char *p, int bus, int addr)
{
    snprintf(p, 512, "%s/i2c-%d/%02x", R, bus, addr);
}

static void mkdev(int bus, int addr, const uint8_t *regs)
{
    char p[512];
    snprintf(p, sizeof(p), "%s/i2c-%d", R, bus);
    mkdir(p, 0755);
    path(p, bus, addr);
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (write(fd, regs, 256) != 256) abort();
    close(fd);
}

static void rmdev(int bus, int addr)
{
    char p[512];
    path(p, bus, addr);
    unlink(p);
}

static void hide(int bus, int addr, int on)
{
    char p[512], q[520];
    path(p, bus, addr);
    snprintf(q, sizeof(q), "%s.hidden", p);
    if (on) rename(p, q);
    else rename(q, p);
}

static int reg(int bus, int addr, int r)
{
    char p[512];
    uint8_t b;
    path(p, bus, addr);
    int fd = open(p, O_RDONLY);
    if (fd < 0) return -1;
    int n = pread(fd, &b, 1, r);
    close(fd);
    return n == 1 ? b : -1;
}

static void setreg(int bus, int addr, int r, int v)
{
    char p[512];
    uint8_t b = v;
    path(p, bus, addr);
    int fd = open(p, O_WRONLY);
    if (fd < 0 || pwrite(fd, &b, 1, r) != 1) abort();
    close(fd);
}

static void pcs_temp(int addr, double t)
{
    unsigned raw = (unsigned)lround(t * 8);
    setreg(5, addr, 0, raw >> 8);
    setreg(5, addr, 1, raw & 0xff);
}

static void vrm_temp(int addr, double t)
{
    int w = (int)lround(t) & 0x7ff; /* LINEAR11, exponent 0 */
    setreg(6, addr, 0x8d, w & 0xff);
    setreg(6, addr, 0x8e, w >> 8);
}

static void iohub_temp(double t)
{
    setreg(2, 0x4c, 0x01, (int)t);
    setreg(2, 0x4c, 0x10, (int)((t - (int)t) * 8) << 5);
}

static void cpu_rpm(double rpm)
{
    int count = rpm <= 0 ? 0xfffc : (int)(5400000 / rpm) & 0xfffc;
    setreg(6, 0x4c, 0x46, count & 0xff);
    setreg(6, 0x4c, 0x47, count >> 8);
}

/* as read on the board (6-004c, 2-004c): PWM_FREQ 0x17 (100 % = 46), the
 * 1.4 kHz clock, manual; the fake keeps 6 bits of 0x4C and latches the
 * tachometer MSB on reading the LSB, as the chip (see fand-hw.c) */
static void lm96163(int bus, double remote)
{
    uint8_t r[256] = { 0 };
    r[0xfe] = 0x01;
    r[0xff] = 0x49;
    /* the healthy setup, as dumped from 6-004c on the board */
    r[0x03] = r[0x09] = 0x06;
    r[0x04] = r[0x0a] = 0x08;
    r[0x05] = r[0x0b] = 0x46;
    r[0x07] = r[0x0d] = 0x69;
    r[0x16] = 0xa4;
    r[0x19] = 0x6e;
    r[0x21] = 0x0a;
    r[0x45] = 0x68;
    r[0x4a] = 0x2b;
    r[0x4b] = 0x3f;
    r[0x4c] = 0x2e;
    r[0x4d] = 0x17;
    r[0x4f] = 0x04;
    r[0x50] = 0x7f;
    r[0x51] = 0x3f;
    r[0xbf] = 0x01;
    r[0x46] = 0xc7; /* 0x07c7: 2716 rpm */
    r[0x47] = 0x07;
    r[0x48] = r[0x49] = 0xff;
    r[0x01] = (int)remote;
    mkdev(bus, 0x4c, r);
    char p[512];
    snprintf(p, sizeof(p), "%s/i2c-%d/4c.lm96163", R, bus);
    close(open(p, O_CREAT | O_WRONLY, 0644));
    snprintf(p, sizeof(p), "%s/i2c-%d/4c.latch", R, bus);
    unlink(p);
}

static void emc2305(int addr)
{
    uint8_t r[256] = { 0 };
    r[0xfd] = 0x34;
    r[0xfe] = 0x5d;
    r[0x20] = 0x40;
    for (int n = 0; n < 5; n++)
    {
        r[0x30 + 0x10 * n] = 0xff;
        r[0x32 + 0x10 * n] = 0x2b;
        r[0x3e + 0x10 * n] = 0xff;
        r[0x3f + 0x10 * n] = 0xf8;
    }
    mkdev(2, addr, r);
}

/* the board as measured: idle temperatures, fans at full speed */
static void board(void)
{
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s/leds/platform:red:fanfail && echo none > %s/leds/platform:red:fanfail/trigger",
             R, R, R);
    if (system(cmd)) abort();
    lm96163(6, 39);
    lm96163(2, 83);
    emc2305(0x2c);
    emc2305(0x2e);
    uint8_t z[256] = { 0 };
    for (int a = 0x20; a <= 0x26; a++)
    {
        mkdev(5, a, z);
        pcs_temp(a, 50 + (a - 0x20));
    }
    for (int a = 0x44; a <= 0x46; a++)
    {
        mkdev(6, a, z);
        vrm_temp(a, 65);
    }
    cpu_rpm(2000);
    hw_reset();
}

static char *led(void)
{
    static char s[64];
    char p[512];
    snprintf(p, sizeof(p), "%s/leds/platform:red:fanfail/trigger", R);
    FILE *f = fopen(p, "r");
    s[0] = 0;
    if (f)
    {
        if (!fgets(s, sizeof(s), f)) s[0] = 0;
        fclose(f);
    }
    s[strcspn(s, "\n")] = 0;
    return s;
}

/* duty % as the chips have it */
static double cpu_duty(void)
{
    return reg(6, 0x4c, 0x4c) * 100.0 / 46;
}

static double case_duty(int addr, int n)
{
    return reg(2, addr, 0x30 + 0x10 * n) * 100.0 / 255;
}

static int all_full(void)
{
    if (reg(6, 0x4c, 0x4c) != 46 || reg(2, 0x4c, 0x4c) != 46) return 0;
    for (int a = 0x2c; a <= 0x2e; a += 2)
        for (int n = 0; n < 5; n++)
            if (reg(2, a, 0x30 + 0x10 * n) != 0xff) return 0;
    return 1;
}

static void run(struct fand *f, double sec, int host)
{
    for (int i = 0; i < (int)(sec * 1000 / f->c.poll_ms + 0.5); i++)
    {
        T += f->c.poll_ms;
        fand_step(f, T, host);
    }
}

/* the defaults, but the CPU fan's duty is the zone's (no range): the
 * curve tests read it off the chip; t_fan_range tests the ranges */
static struct config cfg_test(void)
{
    struct config c;
    cfg_defaults(&c);
    c.cpu_fan_min = c.floor;
    c.cpu_fan_max = 100;
    return c;
}

static void fan_model(const char *what)
{
    char p[512];
    snprintf(p, sizeof(p), "%s/i2c-6/4c.fan", R);
    if (!what)
    {
        unlink(p);
        return;
    }
    FILE *f = fopen(p, "w");
    fputs(what, f);
    fclose(f);
}

/* a daemon that has the host on and its start behind it */
static void start(struct fand *f, const struct config *c)
{
    board();
    T = 1000000;
    fand_init(f, c, T);
    f->on_since_hint = T - 100000; /* on for a while: no settle */
    run(f, c->start_full_ms / 1000.0 + 1, HOST_ON);
}

/* takeover of whatever the chips hold now */
static void start_keep(struct fand *f, const struct config *c)
{
    T += 1000;
    fand_init(f, c, T);
    f->on_since_hint = T - 100000;
    run(f, 2, HOST_ON);
}

/* ------------------------------------------------------------------ tests */
static void t_curves(void)
{
    struct curve cv;
    char e[128];
    CHECK(!curve_parse("40:30 60:50 80:100", &cv, 30, e, sizeof(e)), "parse %s", e);
    CHECK(cv.n == 3, "3 points");
    CHECK(curve_eval(&cv, 20) == 30, "below the first point: its duty");
    CHECK(curve_eval(&cv, 50) == 40, "interpolated");
    CHECK(curve_eval(&cv, 70) == 75, "interpolated 2");
    CHECK(curve_eval(&cv, 95) == 100, "above the last point: 100");
    CHECK(curve_eval(&cv, NAN) == 100, "no temperature: 100");
    CHECK(!curve_parse("40:30, 75:100", &cv, 30, e, sizeof(e)), "commas");
    static const char *bad[] = {
        "40:20 75:100",       /* below the floor */
        "40:30 75:101",       /* above 100 */
        "50:30 40:100",       /* temperatures not rising */
        "40:30 40:100",       /* equal temperatures */
        "40:60 50:40 75:100", /* duty falls */
        "40:30 70:90",        /* never full speed */
        "40:30",              /* one point */
        "",                   /* none */
        "abc",
        "40-30 75:100",
        "40:30x 75:100",
        "-5:30 75:100",
        "40:30 130:100",
        "10:30 20:30 30:30 40:30 50:30 60:30 70:30 80:30 90:100",
        "40:nan 75:100",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        CHECK(curve_parse(bad[i], &cv, 30, e, sizeof(e)), "rejects \"%s\"", bad[i]);
    CHECK(!curve_parse("10:30 20:30 30:30 40:30 50:30 60:30 70:30 80:100", &cv, 30, e, sizeof(e)), "8 points");
    char s[160];
    curve_parse("45:30 55.5:38 75:100", &cv, 30, e, sizeof(e));
    curve_format(&cv, s, sizeof(s));
    CHECK(!strcmp(s, "45:30 55.5:38 75:100"), "format %s", s);
}

static void t_config(void)
{
    struct config c;
    char err[FAND_MAX_ERR][128];
    cfg_defaults(&c);
    CHECK(c.floor == 30 && c.use[Z_CPU][G_PCS] && c.use[Z_BOARD][G_IOHUB] && !c.use[Z_CPU][G_IOHUB], "defaults");
    for (int z = 0; z < NZONES; z++)
        for (int g = 0; g < NGROUPS; g++)
            if (c.use[z][g])
                CHECK(c.curve[z][g].d[0] >= c.floor && c.curve[z][g].d[c.curve[z][g].n - 1] == 100, "default curve %d/%d", z, g);

    cfg_defaults(&c);
    int n = cfg_parse(&c, "# comment\n\nFLOOR=35\nHYSTERESIS=2.5\nCPU_PCS_CURVE=\"40:35 70:100\"\nSLEW_DOWN = 2\n", err, FAND_MAX_ERR);
    CHECK(n == 0 && c.floor == 35 && c.hyst == 2.5 && c.slew_down == 2 && c.curve[Z_CPU][G_PCS].n == 2, "good file (%d: %s)", n,
          n ? err[0] : "");

    /* the curve line before FLOOR: still checked against it */
    cfg_defaults(&c);
    struct curve before = c.curve[Z_CPU][G_PCS];
    n = cfg_parse(&c, "CPU_PCS_CURVE=40:30 75:100\nFLOOR=40\n", err, FAND_MAX_ERR);
    CHECK(n == 1 && c.floor == 40 && c.curve[Z_CPU][G_PCS].n == before.n, "curve below a later FLOOR rejected (%d)", n);

    cfg_defaults(&c);
    n = cfg_parse(&c, "FLOOR=10\n", err, FAND_MAX_ERR);
    CHECK(n == 1 && c.floor == 30, "FLOOR below 20 rejected: %s", n ? err[0] : "");
    cfg_defaults(&c);
    n = cfg_parse(&c, "FLOOR=101\nHYSTERESIS=20\nSLEW_UP=1\nPOLL_MS=10\nMANUAL_TIMEOUT_SEC=0\nWHATEVER=1\nnot a line\n", err,
                  FAND_MAX_ERR);
    CHECK(n == 7 && c.floor == 30 && c.hyst == 3 && c.slew_up == 10 && c.poll_ms == 1000 && c.manual_timeout_ms == 600000,
          "bad values rejected, defaults kept (%d)", n);
    cfg_defaults(&c);
    n = cfg_parse(&c, "CPU_PCS_CURVE=40:25 75:100\nBOARD_IOHUB_CURVE=80:50 90:90\nCPU_PCS_CURVE=none\n", err, FAND_MAX_ERR);
    CHECK(n == 3 && c.use[Z_CPU][G_PCS], "curves below floor / not reaching 100 / PCS none rejected (%d)", n);
    cfg_defaults(&c);
    n = cfg_parse(&c, "BOARD_VRM_CURVE=none\nCASE_FANS=none\nVRM_ADDRS=none\nIOHUB=none\n", err, FAND_MAX_ERR);
    CHECK(n == 0 && !c.use[Z_BOARD][G_VRM] && c.ncase == 0 && c.nvrm == 0 && c.iohub.bus < 0, "none (%d %s)", n, n ? err[0] : "");
    cfg_defaults(&c);
    n = cfg_parse(&c, "PCS_ADDRS=none\nCPU_FAN=6:0x99\nCASE_FANS=2:0x2c 2:0x2e 2:0x2f\nPCS_ADDRS=0x20 0x21\n", err, FAND_MAX_ERR);
    CHECK(n == 3 && c.npcs == 2 && c.cpu_fan.addr == 0x4c && c.ncase == 2, "addresses (%d)", n);
    cfg_defaults(&c);
    n = cfg_parse(&c, "POLL_MS=3000\nSTALE_SEC=2\n", err, FAND_MAX_ERR);
    CHECK(n == 1 && c.stale_ms == 6000, "STALE_SEC at least 2 polls (%d)", c.stale_ms);
    cfg_defaults(&c);
    n = cfg_parse(&c, "CPU_FAN_TACH=no\nEMC_WATCHDOG=maybe\nHOST_PROBE=off\n", err, FAND_MAX_ERR);
    CHECK(n == 1 && !c.cpu_tach && c.emc_watchdog && !c.host_probe, "booleans (%d)", n);

    /* saving a key: replaced in place, comments kept, appended when new */
    char file[300];
    snprintf(file, sizeof(file), "%s.conf", R);
    FILE *fp = fopen(file, "w");
    fputs("# my fans\nFLOOR=35\n  CPU_PCS_CURVE = \"40:35 75:100\"\nSLEW_DOWN=2", fp);
    fclose(fp);
    CHECK(!cfg_save_key(file, "CPU_PCS_CURVE", "45:40 70:100"), "save");
    CHECK(!cfg_save_key(file, "BOARD_IOHUB_CURVE", "80:40 95:100"), "save new");
    cfg_defaults(&c);
    n = cfg_load(&c, file, err, FAND_MAX_ERR);
    CHECK(n == 0 && c.floor == 35 && c.slew_down == 2 && c.curve[Z_CPU][G_PCS].t[0] == 45 && c.curve[Z_BOARD][G_IOHUB].t[0] == 80,
          "saved file reads back (%d %s)", n, n ? err[0] : "");
    char text[1024];
    fp = fopen(file, "r");
    size_t len = fread(text, 1, sizeof(text) - 1, fp);
    text[len] = 0;
    fclose(fp);
    CHECK(strstr(text, "# my fans\n") && !strstr(text, "40:35"), "comment kept, old value gone");
    unlink(file);
    cfg_defaults(&c);
    CHECK(cfg_load(&c, "/nonexistent/reimu-fand.conf", err, FAND_MAX_ERR) == 0, "no file: defaults");
}

static void t_host(void)
{
    struct fand f;
    struct config c = cfg_test();
    board();
    /* fans as some earlier owner left them */
    setreg(6, 0x4c, 0x4c, 10);
    T = 5000000;
    fand_init(&f, &c, T);
    unsigned long x0 = hw_transfers();
    run(&f, 3, HOST_PENDING);
    CHECK(hw_transfers() == x0, "host state not known yet: no I2C");
    run(&f, 60, HOST_OFF);
    CHECK(hw_transfers() == x0 && f.phase == PH_OFF, "host off: no I2C at all");
    run(&f, 30, HOST_ON); /* the first poll that sees it on starts the count */
    CHECK(hw_transfers() == x0 && f.phase == PH_SETTLE, "powering up: bus left alone for SETTLE_SEC");
    CHECK(reg(6, 0x4c, 0x4c) == 10, "untouched");
    run(&f, 1, HOST_ON);
    CHECK(f.phase == PH_RUN && all_full(), "after SETTLE_SEC: control, everything 100%%");
    run(&f, 14, HOST_ON);
    CHECK(all_full(), "100%% for START_FULL_SEC");
    run(&f, 5, HOST_ON);
    double d = cpu_duty();
    CHECK(d < 100 && d > 90, "then down, slowly (%.1f)", d);
    run(&f, 200, HOST_ON);
    /* PCS max 56 °C: 38 + (56-55)/5*9 = 39.8 %; VRMs 65: 30 */
    CHECK(NEAR(f.z[Z_CPU].duty, 39.8, 0.01) && NEAR(cpu_duty(), 39.8, 100.0 / 46), "cpu settles at its demand (%.1f, %.1f)",
          f.z[Z_CPU].duty, cpu_duty());
    /* board: IOHUB 83: 30 + 8/10*20 = 46 %; PCS 56: 30 + 6/10*10 = 36 */
    CHECK(NEAR(f.z[Z_BOARD].duty, 46, 0.01) && NEAR(case_duty(0x2e, 4), 46, 0.4), "board settles at IOHUB's demand (%.1f %.1f)",
          f.z[Z_BOARD].duty, case_duty(0x2e, 4));
    CHECK(!strcmp(led(), "none") && !fand_alarm(&f), "no alarm: LED off");

    run(&f, 1, HOST_OFF);
    CHECK(f.phase == PH_OFF && all_full(), "power off: one fail-safe write (in case it isn't really off)");
    setreg(6, 0x4c, 0x4c, 10);
    x0 = hw_transfers();
    run(&f, 30, HOST_OFF);
    CHECK(hw_transfers() == x0 && reg(6, 0x4c, 0x4c) == 10, "then nothing");
    /* chips power up in their defaults */
    board();
    setreg(6, 0x4c, 0x4c, 0);
    run(&f, 10, HOST_ON);
    CHECK(f.phase == PH_SETTLE && reg(6, 0x4c, 0x4c) == 0, "on again: settling");
    run(&f, 21, HOST_ON);
    CHECK(f.phase == PH_RUN && all_full(), "and 100%% again");
    /* unknown (D-Bus can't say, the devices answer) works as on */
    run(&f, 100, HOST_UNKNOWN);
    CHECK(f.phase == PH_RUN && f.z[Z_CPU].duty < 100, "unknown = on");

    /* a restarted daemon: the host has been on for long, no settle */
    board();
    T += 1000;
    fand_init(&f, &c, T);
    f.on_since_hint = T - 600000;
    run(&f, 1, HOST_ON);
    CHECK(f.phase == PH_RUN && all_full(), "restart with the host on: control at once, 100%%");
    /* hint from the future (other boot): ignored, settles */
    fand_init(&f, &c, T);
    f.on_since_hint = T + 600000;
    run(&f, 1, HOST_ON);
    CHECK(f.phase == PH_SETTLE, "bad hint: settles");

    /* probing */
    board();
    CHECK(fand_probe(&c) == 1, "probe: devices answer");
    rmdev(6, 0x4c);
    rmdev(2, 0x4c);
    CHECK(fand_probe(&c) == 1, "probe: PCS answers");
    rmdev(5, 0x20);
    CHECK(fand_probe(&c) == 0, "probe: nothing answers");
}

static void t_hysteresis_slew(void)
{
    struct fand f;
    struct config c = cfg_test();
    c.slew_down = 100; /* see the demand at once */
    c.slew_up = 100;
    start(&f, &c);
    pcs_temp(0x20, 60);
    run(&f, 2, HOST_ON);
    CHECK(NEAR(f.z[Z_CPU].in[G_PCS].demand, 47, 0.01), "60 °C: 47%% (%.2f)", f.z[Z_CPU].in[G_PCS].demand);
    pcs_temp(0x20, 62);
    run(&f, 2, HOST_ON);
    double d62 = 47 + 2 * 13 / 5.0;
    CHECK(NEAR(f.z[Z_CPU].duty, d62, 0.01), "62 °C: up at once (%.2f)", f.z[Z_CPU].duty);
    pcs_temp(0x20, 59.5);
    run(&f, 2, HOST_ON);
    CHECK(NEAR(f.z[Z_CPU].duty, d62, 0.01), "falls 2.5 °C (< hysteresis 3): duty kept (%.2f)", f.z[Z_CPU].duty);
    pcs_temp(0x20, 58.5);
    run(&f, 2, HOST_ON);
    CHECK(NEAR(f.z[Z_CPU].in[G_PCS].t_eff, 61.5, 0.01) && f.z[Z_CPU].duty < d62, "falls 3.5 °C: follows at T+3 (%.2f, %.2f)",
          f.z[Z_CPU].in[G_PCS].t_eff, f.z[Z_CPU].duty);
    pcs_temp(0x20, 59);
    run(&f, 2, HOST_ON);
    CHECK(NEAR(f.z[Z_CPU].in[G_PCS].t_eff, 61.5, 0.01), "small rise within the band: unchanged");

    /* slew */
    c = cfg_test();
    start(&f, &c);
    pcs_temp(0x20, 50);
    run(&f, 300, HOST_ON);
    double d0 = f.z[Z_CPU].duty;
    pcs_temp(0x20, 74);
    run(&f, 1, HOST_ON);
    CHECK(NEAR(f.z[Z_CPU].duty, d0 + 10, 0.01), "up: SLEW_UP %%/s (%.1f -> %.1f)", d0, f.z[Z_CPU].duty);
    run(&f, 10, HOST_ON);
    double dhi = f.z[Z_CPU].duty;
    CHECK(NEAR(dhi, 96, 0.01), "reaches the demand (%.1f)", dhi);
    pcs_temp(0x20, 50);
    run(&f, 10, HOST_ON);
    CHECK(NEAR(f.z[Z_CPU].duty, dhi - 10, 0.01), "down: SLEW_DOWN %%/s (%.1f)", f.z[Z_CPU].duty);

    /* the floor, never 0 */
    c = cfg_test();
    c.floor = 20;
    c.cpu_fan_min = 20;
    for (int z = 0; z < NZONES; z++)
        for (int g = 0; g < NGROUPS; g++)
            if (c.use[z][g])
            {
                char e[128];
                curve_parse("60:20 90:100", &c.curve[z][g], 20, e, sizeof(e));
            }
    start(&f, &c);
    for (int a = 0x20; a <= 0x26; a++) pcs_temp(a, 20);
    for (int a = 0x44; a <= 0x46; a++) vrm_temp(a, 20);
    iohub_temp(20);
    run(&f, 400, HOST_ON);
    CHECK(f.z[Z_CPU].duty == 20 && reg(6, 0x4c, 0x4c) == 9 && reg(2, 0x2c, 0x30) == 51, "cold: the floor, not 0 (%d %d)",
          reg(6, 0x4c, 0x4c), reg(2, 0x2c, 0x30));
}

static void t_failsafe_inputs(void)
{
    struct fand f;
    struct config c = cfg_test();
    start(&f, &c);
    run(&f, 300, HOST_ON);
    double cpu0 = f.z[Z_CPU].duty, board0 = f.z[Z_BOARD].duty;
    CHECK(cpu0 < 50 && board0 < 50, "quiet at idle (%.1f %.1f)", cpu0, board0);

    /* a PCS sensor stops answering */
    hide(5, 0x23, 1);
    run(&f, 4, HOST_ON);
    CHECK(f.z[Z_CPU].duty == cpu0 && !f.z[Z_CPU].failsafe, "a missed reading or four: no change");
    run(&f, 2, HOST_ON);
    CHECK(f.z[Z_CPU].failsafe && f.z[Z_BOARD].failsafe && all_full(), "missing for STALE_SEC: both zones 100%% at once");
    CHECK(!strcmp(led(), "default-on") && fand_alarm(&f), "LED on");
    CHECK(strstr(f.z[Z_CPU].reason, "0x23") != NULL, "reason names it: %s", f.z[Z_CPU].reason);
    hide(5, 0x23, 0);
    run(&f, 1, HOST_ON);
    CHECK(!f.z[Z_CPU].failsafe && f.z[Z_CPU].duty >= 99 && !strcmp(led(), "none"), "back: automatic again, from 100%%");
    run(&f, 10, HOST_ON);
    CHECK(f.z[Z_CPU].duty < 100, "slewing down");

    /* reads 0: "no sensor" */
    run(&f, 300, HOST_ON);
    pcs_temp(0x25, 0);
    run(&f, 6, HOST_ON);
    CHECK(f.z[Z_CPU].failsafe && all_full(), "PCS reading 0 for STALE_SEC: 100%%");
    pcs_temp(0x25, 55);
    run(&f, 300, HOST_ON);

    /* a VRM out of range (garbage) */
    setreg(6, 0x45, 0x8d, 0xff);
    setreg(6, 0x45, 0x8e, 0x03); /* 1023 °C */
    run(&f, 6, HOST_ON);
    CHECK(f.z[Z_CPU].failsafe && f.z[Z_BOARD].failsafe, "VRM reading out of range: both zones 100%%");
    vrm_temp(0x45, 65);
    run(&f, 300, HOST_ON);

    /* IOHUB diode open: only the board zone */
    setreg(2, 0x4c, 0x02, 0x04);
    run(&f, 6, HOST_ON);
    CHECK(f.z[Z_BOARD].failsafe && !f.z[Z_CPU].failsafe && f.z[Z_BOARD].duty == 100 && f.z[Z_CPU].duty < 100,
          "IOHUB diode fault: the board zone 100%%, the CPU zone not");
    CHECK(reg(2, 0x2c, 0x30) == 0xff && reg(2, 0x4c, 0x4c) == 46 && reg(6, 0x4c, 0x4c) != 46, "on the chips too");
    setreg(2, 0x4c, 0x02, 0);
    run(&f, 300, HOST_ON);

    /* every input gone (host powering off with the state not yet known) */
    for (int a = 0x20; a <= 0x26; a++) hide(5, a, 1);
    run(&f, 6, HOST_ON);
    CHECK(all_full(), "no PCS at all: 100%%");
    for (int a = 0x20; a <= 0x26; a++) hide(5, a, 0);
    run(&f, 1, HOST_ON);

    /* the IOHUB chip isn't an LM96163 */
    setreg(2, 0x4c, 0xff, 0x00);
    run(&f, 6, HOST_ON);
    CHECK(f.z[Z_BOARD].failsafe && f.z[Z_BOARD].duty == 100, "IOHUB chip with a wrong ID: no reading, board zone 100%%");
    setreg(2, 0x4c, 0xff, 0x49);
    fand_clear_alarms(&f);
    run(&f, 2, HOST_ON);
    CHECK(!f.z[Z_BOARD].failsafe && !f.out_alarm, "right again");
}

static void t_outputs(void)
{
    struct fand f;
    struct config c = cfg_test();
    /* the board, 2026-10-24: 0xff written to 0x4C reads back 0x3f, "not
     * driven", and the tachometer was then never read: "0 rpm" */
    board();
    setreg(6, 0x4c, 0x46, 0xc7);
    setreg(6, 0x4c, 0x47, 0x07);
    T = 1000000;
    fand_init(&f, &c, T);
    f.on_since_hint = T - 100000;
    run(&f, 3, HOST_ON);
    CHECK(reg(6, 0x4c, 0x4c) == 46 && reg(2, 0x4c, 0x4c) == 46, "100%% is 2 * PWM_FREQ = 46 (%d %d)", reg(6, 0x4c, 0x4c),
          reg(2, 0x4c, 0x4c));
    CHECK(f.z[Z_CPU].out[0].ok && f.z[Z_BOARD].out[0].ok, "driven and read back");
    CHECK(NEAR(f.z[Z_CPU].out[0].rpm, 5400000.0 / 0x07c4, 1), "tachometer 0x07c7: %.0f rpm", f.z[Z_CPU].out[0].rpm);
    run(&f, 20, HOST_ON);
    CHECK(!f.out_alarm && !f.tach_alarm && !fand_alarm(&f), "no alarm");
    run(&f, 300, HOST_ON);
    CHECK(reg(6, 0x4c, 0x4c) == 18 && reg(6, 0x4c, 0x4c) < 46, "quiet: %d of 46", reg(6, 0x4c, 0x4c));
    /* another PWM_FREQ configured: the scale follows */
    struct config c2 = c;
    c2.pwm_freq = 0x10;
    fand_reconfig(&f, &c2);
    run(&f, 1, HOST_ON);
    CHECK(reg(6, 0x4c, 0x4c) == (int)lround(f.z[Z_CPU].duty * 32 / 100), "PWM_FREQ 0x10: %d of 32", reg(6, 0x4c, 0x4c));
    /* the high-resolution mode: 8 bits, 255 = 100 % */
    setreg(6, 0x4c, 0x45, 0x10);
    c2.pwm_freq = 8;
    c2.pwm_fast = 1;
    fand_reconfig(&f, &c2);
    run(&f, 1, HOST_ON);
    CHECK(reg(6, 0x4c, 0x4c) == (int)lround(f.z[Z_CPU].duty * 255 / 100) && f.z[Z_CPU].out[0].ok, "high resolution: %d of 255",
          reg(6, 0x4c, 0x4c));
    fand_failsafe(&c);
    CHECK(reg(6, 0x4c, 0x4c) == 255, "fail-safe, high resolution: 255");
    setreg(6, 0x4c, 0x45, 0x00);
    setreg(6, 0x4c, 0x4a, 0x2b);
    setreg(6, 0x4c, 0x4d, 0x17);
    fand_reconfig(&f, &c);
    fand_failsafe(&c);
    CHECK(reg(6, 0x4c, 0x4c) == 46, "fail-safe: 46");
    /* the latch: a reader that took the MSB first would get a stale one */
    setreg(6, 0x4c, 0x46, 0x10);
    setreg(6, 0x4c, 0x47, 0x0e); /* 0x0e10: 1500 rpm */
    run(&f, 10, HOST_ON);
    CHECK(NEAR(f.z[Z_CPU].out[0].rpm, 1500, 1) && NEAR(f.z[Z_CPU].out[0].rpm_last, 1500, 1),
          "tachometer follows: %.0f rpm (best of the window)", f.z[Z_CPU].out[0].rpm);

    start(&f, &c);
    run(&f, 300, HOST_ON);
    CHECK(reg(2, 0x2c, 0x20) & 0x20, "EMC2305 watchdog on (continuous)");
    CHECK(reg(2, 0x2e, 0x20) == 0x60, "the other bits kept (0x%02x)", reg(2, 0x2e, 0x20));

    /* someone switched the CPU fan chip to its lookup table */
    setreg(6, 0x4c, 0x4a, 0x0b);
    run(&f, 1, HOST_ON);
    CHECK(reg(6, 0x4c, 0x4a) == 0x2a, "manual mode set again, other bits kept, tach mode accurate (0x%02x)", reg(6, 0x4c, 0x4a));
    /* an EMC2305 channel in RPM mode */
    emc2305(0x2c);
    setreg(2, 0x2c, 0x52, 0xab);
    f.z[Z_BOARD].out[1].ident = 0;
    run(&f, 1, HOST_ON);
    CHECK(reg(2, 0x2c, 0x52) == 0x2b, "RPM mode off (0x%02x)", reg(2, 0x2c, 0x52));

    /* an EMC2305 gone: the others to 100 % */
    hide(2, 0x2e, 1);
    run(&f, 5, HOST_ON);
    CHECK(!f.out_alarm, "not yet");
    run(&f, 2, HOST_ON);
    CHECK(f.out_alarm && reg(6, 0x4c, 0x4c) == 46 && reg(2, 0x2c, 0x30) == 0xff && reg(2, 0x4c, 0x4c) == 46,
          "a fan chip not driven for STALE_SEC: everything else 100%%");
    CHECK(!strcmp(led(), "default-on"), "LED on");
    hide(2, 0x2e, 0);
    run(&f, 1, HOST_ON);
    CHECK(!f.out_alarm && !strcmp(led(), "none"), "back: alarm off");

    /* a write that doesn't read back (chip stuck) */
    run(&f, 300, HOST_ON);
    char p[512];
    snprintf(p, sizeof(p), "%s/i2c-6/4c.ro", R);
    setreg(6, 0x4c, 0x4c, 0x05);
    close(open(p, O_CREAT | O_WRONLY, 0644));
    run(&f, 12, HOST_ON);
    CHECK(f.out_alarm, "CPU fan chip: writes acknowledged but not taken: alarm");
    CHECK(!f.tach_alarm && f.z[Z_CPU].out[0].rpm > 1900, "its tachometer still read (%.0f rpm), no false fan failure",
          f.z[Z_CPU].out[0].rpm);
    unlink(p);
    run(&f, 1, HOST_ON);
    CHECK(!f.out_alarm, "taken again");

    /* not an EMC2305 at that address: never driven */
    uint8_t other[256];
    memset(other, 0x11, sizeof(other));
    mkdev(2, 0x2e, other);
    f.z[Z_BOARD].out[2].ident = 0;
    run(&f, 1, HOST_ON);
    CHECK(f.out_alarm && reg(2, 0x2e, 0x30) == 0x11 && reg(2, 0x2e, 0x20) == 0x11, "wrong chip: alarm, nothing written");
    emc2305(0x2e);
    fand_clear_alarms(&f);
    run(&f, 1, HOST_ON);
    CHECK(!f.out_alarm, "right chip again");

    /* the fail-safe (exit, ExecStopPost) */
    run(&f, 300, HOST_ON);
    CHECK(!all_full(), "running quietly");
    setreg(6, 0x4c, 0x4a, 0x0b);
    fand_failsafe(&c);
    CHECK(all_full() && reg(6, 0x4c, 0x4a) == 0x2b, "fail-safe: everything 100%%, manual mode");
    run(&f, 300, HOST_ON);
    rmdev(2, 0x2c);
    fand_failsafe(&c);
    CHECK(reg(6, 0x4c, 0x4c) == 46 && reg(2, 0x4c, 0x4c) == 46 && reg(2, 0x2e, 0x30) == 0xff && reg(2, 0x2e, 0x70) == 0xff,
          "fail-safe with a chip missing: the rest still 100%%");
    memset(other, 0x11, sizeof(other));
    mkdev(2, 0x2c, other);
    fand_failsafe(&c);
    CHECK(reg(2, 0x2c, 0x30) == 0x11, "fail-safe: a wrong chip is not written");
}

static void t_tach(void)
{
    struct fand f;
    struct config c = cfg_test();
    start(&f, &c);
    run(&f, 300, HOST_ON);
    CHECK(NEAR(f.z[Z_CPU].out[0].rpm, 5400000.0 / (2700 & 0xfffc), 1), "rpm read (%.0f)", f.z[Z_CPU].out[0].rpm);
    /* low readings among good ones: the best of the window counts */
    cpu_rpm(100);
    run(&f, 3, HOST_ON);
    cpu_rpm(2000);
    run(&f, 20, HOST_ON);
    CHECK(!f.tach_alarm && !f.tach_verify && !f.tach_checks, "dropouts: no alarm, no check");
    /* slows down below the minimum at 39.8 % (tachometer not trusted there) */
    cpu_rpm(300);
    run(&f, 7, HOST_ON);
    CHECK(!f.tach_alarm && !f.tach_verify, "not yet (window)");
    run(&f, 2, HOST_ON);
    CHECK(f.tach_alarm && !f.tach_checks, "accurate mode, 39.8%% (above TACH_VALID_DUTY 30): alarm directly, no check");
    run(&f, 6, HOST_ON);
    CHECK(f.tach_alarm && all_full() && !strcmp(led(), "default-on"), "slow at full speed too: alarm, all 100%%, LED (<= 15 s)");
    cpu_rpm(2700);
    run(&f, 50, HOST_ON);
    CHECK(f.tach_alarm && all_full(), "kept while not TACH_CLEAR_SEC good");
    run(&f, 15, HOST_ON);
    CHECK(!f.tach_alarm && !strcmp(led(), "none"), "cleared after TACH_CLEAR_SEC good");
    run(&f, 100, HOST_ON);
    CHECK(f.z[Z_CPU].duty < 100, "back to automatic");
    /* stopped: 0xfffc */
    cpu_rpm(0);
    run(&f, 15, HOST_ON);
    CHECK(f.tach_alarm && !f.tach_latched, "stopped: alarm (2nd)");
    cpu_rpm(2700);
    run(&f, 70, HOST_ON);
    run(&f, 100, HOST_ON);
    cpu_rpm(0);
    run(&f, 15, HOST_ON);
    CHECK(f.tach_alarm && f.tach_latched, "3rd alarm within an hour: latched");
    cpu_rpm(2700);
    run(&f, 200, HOST_ON);
    CHECK(f.tach_alarm && all_full(), "latched: stays though the fan is fine");
    run(&f, 1, HOST_OFF);
    CHECK(f.tach_alarm && !strcmp(led(), "default-on"), "latched: survives host off, LED stays");
    run(&f, 31, HOST_ON);
    fand_clear_alarms(&f);
    run(&f, 200, HOST_ON);
    CHECK(!f.tach_alarm && f.z[Z_CPU].duty < 100, "ClearAlarms: automatic again");
    /* the tachometer unreadable (chip answers writes, not 0x46): stopped */
    /* no tach configured */
    c.cpu_tach = 0;
    start(&f, &c);
    cpu_rpm(0);
    run(&f, 100, HOST_ON);
    CHECK(!f.tach_alarm, "CPU_FAN_TACH=no: not checked");
}

/* the X37 fan as measured: noisy at 30 %, dropping to 0 at 40 %, steady
 * from 50 %, saturated from 60 % */
static void t_fan_model(void)
{
    struct fand f;
    struct config c = cfg_test(); /* fan duty = zone duty: SetDuty sets it */
    char e[160];
    start(&f, &c);
    fan_model("model");
    c.manual_timeout_ms = 86400000;
    fand_reconfig(&f, &c);
    static const int duties[] = { 30, 35, 40, 45, 50, 55, 60, 100 };
    for (size_t i = 0; i < sizeof(duties) / sizeof(duties[0]); i++)
    {
        fand_set_duty(&f, Z_CPU, duties[i], e, sizeof(e));
        unsigned checks = f.tach_checks;
        run(&f, 3600, HOST_ON);
        CHECK(!f.tach_alarm && f.tach_checks == checks && f.z[Z_CPU].duty == duties[i],
              "%d%%: an hour of its readings, no false alarm, no check (best %.0f rpm)", duties[i], f.z[Z_CPU].out[0].rpm);
    }
    /* stalls at 30 %: alarm within 15 s */
    fand_set_duty(&f, Z_CPU, 30, e, sizeof(e));
    run(&f, 30, HOST_ON);
    fan_model("stall");
    int t_alarm = 0;
    for (int s = 1; s <= 30 && !f.tach_alarm; s++)
    {
        run(&f, 1, HOST_ON);
        if (f.tach_alarm) t_alarm = s;
    }
    CHECK(t_alarm >= 8 && t_alarm <= 15, "stalled at 30%%: alarm after %d s (8..15)", t_alarm);
    CHECK(all_full(), "every fan 100%%");
    fand_clear_alarms(&f);
    fan_model("model");
    run(&f, 100, HOST_ON);
    CHECK(!f.tach_alarm, "turning again");
    /* stalls at 70 % (tachometer trusted): alarm without the check */
    fand_set_duty(&f, Z_CPU, 70, e, sizeof(e));
    run(&f, 30, HOST_ON);
    unsigned checks = f.tach_checks;
    fan_model("stall");
    t_alarm = 0;
    for (int s = 1; s <= 30 && !f.tach_alarm; s++)
    {
        run(&f, 1, HOST_ON);
        if (f.tach_alarm) t_alarm = s;
    }
    CHECK(t_alarm >= 8 && t_alarm <= 10 && f.tach_checks == checks, "stalled at 70%%: alarm after %d s, no check", t_alarm);
    fan_model(NULL);
}

/* the useful duty range of a fan: zone FLOOR..100 spread over it */
static int stall_time(struct fand *f)
{
    fan_model("stall");
    for (int s = 1; s <= 30; s++)
    {
        run(f, 1, HOST_ON);
        if (f->tach_alarm) return s;
    }
    return 0;
}

/* TACH_MODE: LM96163 0x4A bits 1:0 */
static void t_tach_mode(void)
{
    struct fand f;
    struct config c = cfg_test();
    char e[160], p[512];
    start(&f, &c);
    CHECK(reg(6, 0x4c, 0x4a) == 0x2a && f.z[Z_CPU].out[0].tach_mode_ok, "CPU fan chip: 0x2b -> 0x2a (accurate), read back");
    CHECK(reg(2, 0x4c, 0x4a) == 0x2b, "the chip without a tachometer: untouched");
    CHECK(reg(6, 0x4c, 0x4d) == 0x17, "PWM_FREQ untouched");
    /* the platform powers off and on: the chip is back to 0x2b */
    run(&f, 1, HOST_OFF);
    board();
    run(&f, 31, HOST_ON);
    CHECK(reg(6, 0x4c, 0x4a) == 0x2a && f.z[Z_CPU].out[0].tach_mode_ok, "power on: set again");
    /* polarity and the clock are guarded too */
    setreg(6, 0x4c, 0x4a, 0x33);
    run(&f, 1, HOST_ON);
    CHECK(reg(6, 0x4c, 0x4a) == 0x2a, "0x4A guarded as a whole (0x%02x)", reg(6, 0x4c, 0x4a));
    setreg(6, 0x4c, 0x4a, 0x2b);
    run(&f, 1, HOST_ON);

    /* accurate: the tachometer trusted at the CPU fan's lowest duty; a stall
     * at 40 % alarms directly, no full-speed check */
    fan_model("model");
    c.manual_timeout_ms = 86400000;
    fand_reconfig(&f, &c);
    for (int d = 30; d <= 40; d += 10)
    {
        fand_set_duty(&f, Z_CPU, d, e, sizeof(e));
        unsigned k = f.tach_checks;
        run(&f, 3600, HOST_ON);
        CHECK(!f.tach_alarm && f.tach_checks == k, "accurate, %d%%: an hour, no alarm, no check (best %.0f)", d,
              f.z[Z_CPU].out[0].rpm);
    }
    unsigned k = f.tach_checks;
    int s = stall_time(&f);
    CHECK(s >= 8 && s <= 10 && f.tach_checks == k, "accurate: stall at 40%% alarms after %d s, no check", s);
    fand_clear_alarms(&f);
    fan_model("model");
    run(&f, 100, HOST_ON);

    /* a chip that refuses the mode: the old threshold and the check */
    snprintf(p, sizeof(p), "%s/i2c-6/4c.no-tachmode", R);
    close(open(p, O_CREAT | O_WRONLY, 0644));
    setreg(6, 0x4c, 0x4a, 0x2b);
    run(&f, 2, HOST_ON);
    CHECK(reg(6, 0x4c, 0x4a) == 0x2b && !f.z[Z_CPU].out[0].tach_mode_ok && f.z[Z_CPU].out[0].ok,
          "refused: noted, the fan still driven");
    for (int d = 30; d <= 40; d += 10)
    {
        fand_set_duty(&f, Z_CPU, d, e, sizeof(e));
        k = f.tach_checks;
        run(&f, 3600, HOST_ON);
        CHECK(!f.tach_alarm && f.tach_checks == k, "least effect, %d%%: an hour of its dropouts, no alarm, no check (best %.0f)",
              d, f.z[Z_CPU].out[0].rpm);
    }
    k = f.tach_checks;
    s = stall_time(&f);
    CHECK(s >= 8 && s <= 15 && f.tach_checks == k + 1, "refused: stall at 40%% checked at full speed, alarm after %d s", s);
    fand_clear_alarms(&f);
    unlink(p);
    fan_model(NULL);

    /* TACH_MODE=least */
    struct config l = cfg_test();
    char err[FAND_MAX_ERR][128];
    CHECK(cfg_parse(&l, "TACH_MODE=least\n", err, FAND_MAX_ERR) == 0 && l.tach_mode == 3, "TACH_MODE=least parsed");
    CHECK(cfg_parse(&l, "TACH_MODE=fast\n", err, FAND_MAX_ERR) == 1 && l.tach_mode == 3, "TACH_MODE=fast refused");
    start(&f, &l);
    CHECK(reg(6, 0x4c, 0x4a) == 0x2b && !f.z[Z_CPU].out[0].tach_mode_ok, "least: 0x2b, threshold TACH_VALID_DUTY_OTHER");
    /* the fail-safe doesn't touch the mode bits */
    setreg(6, 0x4c, 0x4a, 0x0a);
    fand_failsafe(&l);
    CHECK(reg(6, 0x4c, 0x4a) == 0x2a, "fail-safe: bit 5 only (0x%02x)", reg(6, 0x4c, 0x4a));
}

/* FLOOR=100 (or any floor at or above a fan's useful maximum): a valid
 * "always full" configuration, not an error */
static void t_floor_full(void)
{
    struct config k;
    char err[FAND_MAX_ERR][128];
    cfg_defaults(&k);
    int n = cfg_parse(&k, "FLOOR=100\n", err, FAND_MAX_ERR);
    CHECK(n == 0 && k.floor == 100 && k.cpu_fan_max == 65 && k.board_fan_max == 100 && k.case_fan_max == 100,
          "FLOOR=100: no error, ranges kept (%d: %s)", n, n ? err[0] : "");
    cfg_defaults(&k);
    n = cfg_parse(&k, "FLOOR=65\n", err, FAND_MAX_ERR);
    CHECK(n == 0, "FLOOR=65 = CPU_FAN_MAX_DUTY: no error (%d: %s)", n, n ? err[0] : "");
    cfg_defaults(&k);
    n = cfg_parse(&k, "FLOOR=80\n", err, FAND_MAX_ERR);
    CHECK(n == 0, "FLOOR=80 above CPU_FAN_MAX_DUTY: no error, the floor wins (%d: %s)", n, n ? err[0] : "");
    cfg_defaults(&k);
    n = cfg_parse(&k, "CPU_FAN_MIN_DUTY=50\nCPU_FAN_MAX_DUTY=50\n", err, FAND_MAX_ERR);
    CHECK(n == 0 && k.cpu_fan_max == 50, "CPU_FAN_MAX_DUTY = CPU_FAN_MIN_DUTY: valid, a fixed duty (%d: %s)", n, n ? err[0] : "");
    cfg_defaults(&k);
    n = cfg_parse(&k, "CPU_FAN_MIN_DUTY=50\nCPU_FAN_MAX_DUTY=40\n", err, FAND_MAX_ERR);
    CHECK(n == 1 && k.cpu_fan_max == 100, "CPU_FAN_MAX_DUTY below its own minimum: an error, 100 (%d)", n);

    struct fand f;
    char e[160];
    cfg_defaults(&k);
    k.floor = 100;
    start(&f, &k);
    run(&f, 300, HOST_ON);
    CHECK(all_full() && f.z[Z_CPU].duty == 100 && !fand_alarm(&f), "FLOOR=100: every fan full scale, no alarm");
    cfg_defaults(&k);
    k.floor = 65;
    start(&f, &k);
    run(&f, 300, HOST_ON);
    fand_set_duty(&f, Z_CPU, 70, e, sizeof(e));
    run(&f, 1, HOST_ON);
    CHECK(reg(6, 0x4c, 0x4c) == 30 && NEAR(f.z[Z_CPU].out[0].duty, 65, 0.01), "FLOOR=65: the CPU fan at its 65%% (%d)",
          reg(6, 0x4c, 0x4c));
    fand_set_duty(&f, Z_CPU, 100, e, sizeof(e));
    run(&f, 1, HOST_ON);
    CHECK(reg(6, 0x4c, 0x4c) == 46, "and 100%%: full scale");
    cfg_defaults(&k);
    k.floor = 80;
    start(&f, &k);
    run(&f, 300, HOST_ON);
    CHECK(reg(6, 0x4c, 0x4c) == 37 && reg(2, 0x2c, 0x30) >= 204, "FLOOR=80: the CPU fan at the floor 80%% (%d), not 65",
          reg(6, 0x4c, 0x4c));
}

/* Турбо: every zone at full scale until it expires or is turned off */
static void t_turbo(void)
{
    struct fand f;
    struct config c;
    char e[160];
    cfg_defaults(&c);
    start(&f, &c);
    run(&f, 300, HOST_ON);
    CHECK(!all_full() && !fand_turbo_left(&f), "quiet, no turbo");
    CHECK(!fand_set_turbo(&f, 15, e, sizeof(e)), "turbo 15 min");
    run(&f, 1, HOST_ON);
    CHECK(all_full() && reg(6, 0x4c, 0x4c) == 46, "turbo: every fan full scale (the CPU fan 46, not its 65%%)");
    CHECK(!strcmp(f.z[Z_CPU].reason, "турбо") && !fand_alarm(&f) && !strcmp(led(), "none"), "reason турбо, no alarm, LED off (%s)",
          f.z[Z_CPU].reason);
    CHECK(fand_turbo_left(&f) == 899, "899 s left (%d)", fand_turbo_left(&f));
    struct buf b = { 0 };
    fand_json(&f, &b);
    CHECK(b.s && strstr(b.s, "\"turbo\":true") && strstr(b.s, "\"turbo_left\":899"), "JSON: turbo, turbo_left");
    free(b.s);
    /* manual mode and its timeout don't matter */
    fand_set_duty(&f, Z_CPU, 40, e, sizeof(e));
    run(&f, 1, HOST_ON);
    CHECK(all_full(), "manual 40%% during turbo: still full");
    run(&f, 660, HOST_ON);
    CHECK(f.z[Z_CPU].mode == MODE_AUTO && all_full() && fand_turbo_left(&f) > 0, "manual timed out, turbo still on");
    run(&f, 239, HOST_ON);
    CHECK(fand_turbo_left(&f) == 0 && !all_full(), "expired after 15 min: back to the curves (slewing down)");
    b = (struct buf){ 0 };
    fand_json(&f, &b);
    CHECK(b.s && strstr(b.s, "\"turbo\":false"), "JSON: turbo false");
    free(b.s);
    /* off before it expires */
    run(&f, 300, HOST_ON);
    fand_set_turbo(&f, 60, e, sizeof(e));
    run(&f, 10, HOST_ON);
    CHECK(all_full(), "turbo 60");
    CHECK(!fand_set_turbo(&f, 0, e, sizeof(e)) && fand_turbo_left(&f) == 0, "SetTurbo 0: off");
    run(&f, 5, HOST_ON);
    CHECK(!all_full(), "off: back to the curves");
    /* limits */
    CHECK(fand_set_turbo(&f, FAND_TURBO_MAX_MIN + 1, e, sizeof(e)) && fand_turbo_left(&f) == 0, "721 min refused (%s)", e);
    CHECK(!fand_set_turbo(&f, FAND_TURBO_UNTIL_OFF, e, sizeof(e)) && fand_turbo_left(&f) == FAND_TURBO_MAX_MIN * 60,
          "until off: capped at 12 h (%d s)", fand_turbo_left(&f));
    run(&f, 12 * 3600 - 1, HOST_ON);
    CHECK(fand_turbo_left(&f) == 1 && all_full(), "11:59:59 later: on");
    run(&f, 2, HOST_ON);
    CHECK(fand_turbo_left(&f) == 0, "12 h: off");
    /* survives a daemon restart (the main loop keeps turbo_until in /run) */
    fand_set_turbo(&f, 30, e, sizeof(e));
    int64_t until = f.turbo_until;
    struct fand g;
    fand_init(&g, &c, T);
    g.on_since_hint = T - 100000;
    g.turbo_until = until;
    run(&g, c.start_full_ms / 1000.0 + 10, HOST_ON);
    CHECK(all_full() && fand_turbo_left(&g) > 1700, "restarted: turbo kept (%d s left)", fand_turbo_left(&g));
    /* host off: no I2C, the clock runs; on again: full */
    run(&g, 60, HOST_OFF);
    CHECK(fand_turbo_left(&g) > 1600 && fand_turbo_left(&g) < 1750, "host off: turbo counts on (%d)", fand_turbo_left(&g));
    unsigned long x = hw_transfers();
    run(&g, 10, HOST_OFF);
    CHECK(hw_transfers() == x, "host off: no I2C");
    run(&g, 31, HOST_ON);
    CHECK(all_full(), "on again: full (turbo)");
    /* the fail-safe still names its cause */
    hide(5, 0x20, 1);
    run(&g, 6, HOST_ON);
    CHECK(g.z[Z_CPU].failsafe && fand_alarm(&g) && strstr(g.z[Z_CPU].reason, "0x20"), "a sensor gone during turbo: alarm, reason");
    hide(5, 0x20, 0);
}

static void gpu_temp(const char *text, int age_s)
{
    char p[512];
    snprintf(p, sizeof(p), "%s/gpu-temp", R);
    if (!text)
    {
        unlink(p);
        return;
    }
    FILE *f = fopen(p, "w");
    fputs(text, f);
    fclose(f);
    struct timeval tv[2];
    gettimeofday(&tv[0], NULL);
    tv[0].tv_sec -= age_s;
    tv[1] = tv[0];
    utimes(p, tv);
}

/* the host's GPU (reimu-hostlink: /run/reimu-hostlink/gpu-temp, m°C, gone
 * when stale): an optional input of the board zone, never a fail-safe */
static void t_gpu(void)
{
    struct config c;
    char err[FAND_MAX_ERR][128];
    cfg_defaults(&c);
    CHECK(!strcmp(c.gpu_file, "/run/reimu-hostlink/gpu-temp") && c.use[Z_BOARD][G_GPU] && !c.use[Z_CPU][G_GPU] &&
              c.curve[Z_BOARD][G_GPU].n == 4 && c.curve[Z_BOARD][G_GPU].t[0] == 65 && c.curve[Z_BOARD][G_GPU].d[3] == 100,
          "defaults: the board zone, 65:30 75:45 85:70 95:100");
    CHECK(curve_key(Z_BOARD, G_GPU) && !strcmp(curve_key(Z_BOARD, G_GPU), "BOARD_GPU_CURVE") && !curve_key(Z_CPU, G_GPU),
          "BOARD_GPU_CURVE, no CPU_GPU_CURVE");
    int n = cfg_parse(&c, "GPU_TEMP_FILE=none\n", err, FAND_MAX_ERR);
    CHECK(n == 0 && !c.gpu_file[0], "GPU_TEMP_FILE=none: off");
    cfg_defaults(&c);
    n = cfg_parse(&c, "BOARD_GPU_CURVE=60:20 90:100\n", err, FAND_MAX_ERR);
    CHECK(n == 1 && c.curve[Z_BOARD][G_GPU].t[0] == 65, "a GPU curve below the floor refused");

    c = cfg_test();
    snprintf(c.gpu_file, sizeof(c.gpu_file), "%.100s/gpu-temp", R);
    struct fand f;
    gpu_temp(NULL, 0);
    start(&f, &c);
    run(&f, 300, HOST_ON);
    double cpu0 = f.z[Z_CPU].duty;
    CHECK(NEAR(f.z[Z_BOARD].duty, 46, 0.01) && !f.z[Z_BOARD].failsafe && !fand_alarm(&f), "no GPU file: IOHUB rules (46%%), no alarm");
    gpu_temp("90000\n", 0);
    run(&f, 10, HOST_ON);
    CHECK(NEAR(f.z[Z_BOARD].in[G_GPU].demand, 85, 0.01) && NEAR(f.z[Z_BOARD].duty, 85, 0.01), "GPU 90 °C: board 85%% (%.1f)",
          f.z[Z_BOARD].duty);
    CHECK(NEAR(f.z[Z_CPU].duty, cpu0, 0.01), "the CPU zone doesn't follow the GPU");
    struct buf b = { 0 };
    fand_json(&f, &b);
    CHECK(b.s && strstr(b.s, "\"id\":\"gpu\"") && strstr(b.s, "\"temp\":90.0"), "JSON: the gpu input, 90.0");
    free(b.s);
    gpu_temp(NULL, 0);
    run(&f, 2, HOST_ON);
    CHECK(!f.z[Z_BOARD].failsafe && !fand_alarm(&f) && !strcmp(led(), "none") && NEAR(f.z[Z_BOARD].demand, 46, 0.01),
          "file gone: no fail-safe, no alarm, IOHUB rules again (%.1f)", f.z[Z_BOARD].demand);
    b = (struct buf){ 0 };
    fand_json(&f, &b);
    CHECK(b.s && strstr(b.s, "\"id\":\"gpu\"") && !strstr(b.s, "\"temp\":90.0"), "JSON: gpu without a temperature");
    free(b.s);
    gpu_temp("95000\n", 60);
    run(&f, 2, HOST_ON);
    CHECK(NEAR(f.z[Z_BOARD].demand, 46, 0.01) && !f.z[Z_BOARD].failsafe, "a 60 s old file: ignored");
    static const char *const bad[] = { "abc\n", "", "-5000\n", "200000\n", "95000abc\n" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
    {
        gpu_temp(bad[i], 0);
        run(&f, 2, HOST_ON);
        CHECK(NEAR(f.z[Z_BOARD].demand, 46, 0.01) && !f.z[Z_BOARD].failsafe, "garbage \"%.8s\": ignored", bad[i]);
    }
    gpu_temp("60000\n", 0);
    run(&f, 2, HOST_ON);
    CHECK(NEAR(f.z[Z_BOARD].demand, 46, 0.01) && NEAR(f.z[Z_BOARD].in[G_GPU].demand, 30, 0.01), "GPU 60 °C (30%%): the max is IOHUB's");
    gpu_temp("97000", 0);
    run(&f, 10, HOST_ON); /* up at SLEW_UP */
    CHECK(f.z[Z_BOARD].duty == 100 && f.z[Z_BOARD].hot && !fand_alarm(&f), "GPU 97 °C, no newline: board full, no alarm");
    gpu_temp(NULL, 0);
    /* off */
    c.gpu_file[0] = 0;
    fand_reconfig(&f, &c);
    gpu_temp("97000\n", 0);
    run(&f, 200, HOST_ON);
    CHECK(f.z[Z_BOARD].duty < 100, "GPU_TEMP_FILE off: not read");
    gpu_temp(NULL, 0);
}

/* a chip from the dump of the board (i2cdump format) */
static int load_dump(int bus, int addr)
{
    static const char *const name = "tests/chips-corrupted-20261101-0147.txt";
    FILE *fp = fopen(name, "r");
    if (!fp) return -1;
    char line[160];
    int in = 0, got = 0;
    uint8_t r[256] = { 0 };
    while (fgets(line, sizeof(line), fp))
    {
        int b, a, row;
        if (sscanf(line, "== bus %d addr 0x%x", &b, &a) == 2)
        {
            in = b == bus && a == addr;
            continue;
        }
        if (!in || sscanf(line, "%x:", &row) != 1 || line[2] != ':') continue;
        for (int i = 0; i < 16; i++) r[row + i] = strtol(line + 4 + 3 * i, NULL, 16);
        got += 16;
    }
    fclose(fp);
    if (got != 256) return -1;
    mkdev(bus, addr, r);
    return 0;
}

static void mark(int bus, int addr, const char *what, int on)
{
    char p[512];
    snprintf(p, sizeof(p), "%s/i2c-%d/%02x.%s", R, bus, addr, what);
    if (on) close(open(p, O_CREAT | O_WRONLY, 0644));
    else unlink(p);
}

/* the setup of the chips guarded: the image found on the board */
static void t_guard(void)
{
    struct fand f;
    struct config c;
    char err[FAND_MAX_ERR][128];
    cfg_defaults(&c);
    CHECK(c.pwm_freq == 0x17 && !c.pwm_fast && c.guard_ms == 5000 && c.guard_corrupt_regs == 4, "defaults: PWM_FREQ pinned at 0x17");
    struct config k = c;
    int n = cfg_parse(&k, "LM96163_PWM_FREQ=0x10\nLM96163_PWM_CLOCK=fast\nGUARD_SEC=2\nGUARD_CORRUPT_REGS=9\n", err, FAND_MAX_ERR);
    CHECK(n == 0 && k.pwm_freq == 0x10 && k.pwm_fast && k.guard_ms == 2000 && k.guard_corrupt_regs == 9, "keys (%d: %s)", n, n ? err[0] : "");
    k = c;
    n = cfg_parse(&k, "LM96163_PWM_FREQ=0\nLM96163_PWM_CLOCK=warp\n", err, FAND_MAX_ERR);
    CHECK(n == 2 && k.pwm_freq == 0x17, "bad PWM settings refused (%d)", n);

    /* a healthy board at takeover: nothing to repair (the tachometer mode is
     * set, as at every power-on) */
    start(&f, &c);
    CHECK(f.repairs == 0 && f.corruptions == 0, "healthy: no repairs (%u)", f.repairs);
    CHECK(reg(6, 0x4c, 0x4a) == 0x2a && reg(2, 0x4c, 0x4a) == 0x2b, "the modes as they should be");

    /* takeover of exactly the image found on the board */
    board();
    CHECK(!load_dump(2, 0x4c) && !load_dump(6, 0x4c) && !load_dump(2, 0x2c) && !load_dump(2, 0x2e), "the dump loaded (cwd: files/)");
    CHECK(reg(2, 0x4c, 0x11) == 0x98 && reg(2, 0x4c, 0x4a) == 0x38 && reg(2, 0x4c, 0x4d) == 0x18, "image as found");
    T = 2000000;
    fand_init(&f, &c, T);
    f.on_since_hint = T - 100000;
    run(&f, 1, HOST_ON);
    static const struct { int r, v; } want[] = { { 0x11, 0 }, { 0x12, 0 }, { 0x03, 6 }, { 0x09, 6 }, { 0x05, 0x46 }, { 0x08, 0 },
        { 0x13, 0 }, { 0x14, 0 }, { 0x19, 0x6e }, { 0x21, 0x0a }, { 0x4a, 0x2b }, { 0x4d, 0x17 } };
    for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++)
        CHECK(reg(2, 0x4c, want[i].r) == want[i].v, "2-004c 0x%02x repaired: 0x%02x", want[i].r, reg(2, 0x4c, want[i].r));
    CHECK(!(reg(2, 0x4c, 0x4a) & 0x10) && reg(2, 0x4c, 0x4c) == 46, "polarity right, 100%% = 46 of the right scale (%d)", reg(2, 0x4c, 0x4c));
    CHECK(f.corruptions == 1 && f.repairs >= 10 && f.z[Z_BOARD].out[0].repairs == f.repairs, "corruption event, %u repairs", f.repairs);
    CHECK(f.z[Z_CPU].out[0].repairs == 0 && f.z[Z_BOARD].out[1].repairs == 0 && f.z[Z_BOARD].out[2].repairs == 0,
          "the healthy chips of the dump: no repairs");
    run(&f, 2, HOST_ON);
    CHECK(f.g[G_IOHUB].valid && f.g[G_IOHUB].max > 55 && f.g[G_IOHUB].max < 58 && !f.z[Z_BOARD].failsafe,
          "IOHUB reads %.2f again, the zone leaves its fail-safe", f.g[G_IOHUB].max);
    unsigned rep = f.repairs;
    run(&f, 300, HOST_ON);
    CHECK(f.repairs == rep && f.corruptions == 1 && !fand_alarm(&f) && f.z[Z_BOARD].duty < 100 && !strcmp(led(), "none"),
          "stays right, no alarm, quiet (%.1f%%)", f.z[Z_BOARD].duty);

    /* the same while running: found by the next poll, the zone never blind */
    double duty = f.z[Z_BOARD].duty;
    CHECK(!load_dump(2, 0x4c), "corrupted while running");
    run(&f, 1, HOST_ON);
    CHECK(reg(2, 0x4c, 0x4a) == 0x2b && reg(2, 0x4c, 0x11) == 0 && reg(2, 0x4c, 0x19) == 0x6e && f.corruptions == 2,
          "next poll: everything repaired (0x4a 0x%02x), corruption event", reg(2, 0x4c, 0x4a));
    run(&f, 10, HOST_ON);
    CHECK(!f.z[Z_BOARD].failsafe && !fand_alarm(&f) && NEAR(f.z[Z_BOARD].duty, duty, 2), "no fail-safe, the duty kept (%.1f)", f.z[Z_BOARD].duty);
    struct buf b = { 0 };
    fand_json(&f, &b);
    char s[64];
    snprintf(s, sizeof(s), "\"repairs\":%u,\"corruptions\":2", f.repairs);
    CHECK(b.s && strstr(b.s, s), "JSON: %s", s);
    free(b.s);

    /* one register: a repair, not a corruption; slow registers within GUARD_SEC */
    start(&f, &c);
    run(&f, 10, HOST_ON);
    setreg(6, 0x4c, 0x19, 0x20);
    run(&f, 6, HOST_ON);
    CHECK(reg(6, 0x4c, 0x19) == 0x6e && f.repairs == 1 && f.corruptions == 0, "T_CRIT repaired within GUARD_SEC, one repair");

    /* the offset: repaired, and that poll's reading isn't trusted */
    start(&f, &c);
    run(&f, 10, HOST_ON);
    setreg(2, 0x4c, 0x11, 0xf0);
    int64_t last = f.g[G_IOHUB].s[0].last_ok;
    run(&f, 1, HOST_ON);
    CHECK(reg(2, 0x4c, 0x11) == 0 && f.g[G_IOHUB].s[0].last_ok == last, "offset repaired first, no reading in that poll");
    run(&f, 1, HOST_ON);
    CHECK(f.g[G_IOHUB].s[0].last_ok == T, "read again the next poll");
    /* an offset that can't be repaired: no data, whatever it reads */
    mark(2, 0x4c, "ro", 1);
    setreg(2, 0x4c, 0x11, 0x02);
    run(&f, 7, HOST_ON);
    CHECK(f.z[Z_BOARD].failsafe && strstr(f.z[Z_BOARD].reason, "offset") && f.z[Z_BOARD].duty == 100, "offset stuck: fail-safe (%s)",
          f.z[Z_BOARD].reason);
    mark(2, 0x4c, "ro", 0);
    run(&f, 2, HOST_ON);
    CHECK(!f.z[Z_BOARD].failsafe, "back");

    /* polarity on the CPU fan chip, the fan model inverted by it: repaired at
     * once, no tachometer conclusion */
    start(&f, &c);
    fan_model("model");
    run(&f, 20, HOST_ON);
    unsigned checks = f.tach_checks;
    setreg(6, 0x4c, 0x4a, 0x3a);
    run(&f, 60, HOST_ON);
    CHECK(reg(6, 0x4c, 0x4a) == 0x2a && !f.tach_alarm && f.tach_checks == checks, "polarity repaired, no fan alarm");
    /* at takeover too, the fan stopped by it */
    run(&f, 1, HOST_OFF);
    board();
    fan_model("model");
    setreg(6, 0x4c, 0x4a, 0x3b);
    run(&f, 31, HOST_ON);
    run(&f, 60, HOST_ON);
    CHECK(reg(6, 0x4c, 0x4a) == 0x2a && !f.tach_alarm && !f.tach_checks, "at takeover: repaired, no fan alarm");
    /* polarity stuck: not driven, and now the alarm is right */
    mark(6, 0x4c, "no-polarity", 1);
    setreg(6, 0x4c, 0x4a, 0x3a);
    run(&f, 30, HOST_ON);
    CHECK(f.out_alarm && !strcmp(led(), "default-on") && !f.z[Z_CPU].out[0].ok, "polarity stuck: not driven, alarm, LED");
    unsigned r2 = f.repairs;
    run(&f, 10, HOST_ON);
    CHECK(f.repairs == r2, "a register that refuses isn't counted");
    mark(6, 0x4c, "no-polarity", 0);
    fan_model(NULL);

    /* the fail-safe must not leave an inverted PWM */
    board();
    setreg(2, 0x4c, 0x4a, 0x38);
    setreg(6, 0x4c, 0x4a, 0x3a);
    fand_failsafe(&c);
    CHECK(!(reg(2, 0x4c, 0x4a) & 0x10) && !(reg(6, 0x4c, 0x4a) & 0x10) && reg(2, 0x4c, 0x4c) == 46, "--failsafe clears the polarity");

    /* a chip with plain registers (QEMU): the write address doesn't work */
    board();
    mark(2, 0x4c, "noalias", 1);
    setreg(2, 0x4c, 0x03, 0x80);
    start_keep(&f, &c);
    CHECK(reg(2, 0x4c, 0x03) == 0x06, "no write alias: the read address (0x%02x)", reg(2, 0x4c, 0x03));
    mark(2, 0x4c, "noalias", 0);

    /* EMC2305 */
    start(&f, &c);
    run(&f, 10, HOST_ON);
    setreg(2, 0x2c, 0x2b, 0x1f);
    setreg(2, 0x2c, 0x2a, 0x03);
    setreg(2, 0x2e, 0x20, 0x40);
    setreg(2, 0x2e, 0x42, 0xab);
    run(&f, 6, HOST_ON);
    CHECK(reg(2, 0x2c, 0x2b) == 0 && reg(2, 0x2c, 0x2a) == 0 && reg(2, 0x2e, 0x20) == 0x60 && reg(2, 0x2e, 0x42) == 0x2b && f.repairs == 4,
          "EMC2305: output, polarity, WD_EN, direct mode repaired (%u)", f.repairs);
    k = c;
    k.emc_watchdog = 0;
    start(&f, &k);
    setreg(2, 0x2e, 0x20, 0x40);
    run(&f, 10, HOST_ON);
    CHECK(reg(2, 0x2e, 0x20) == 0x40, "EMC_WATCHDOG=no: left alone");

    /* --observe: reports, writes nothing */
    board();
    CHECK(!load_dump(2, 0x4c), "observe: the image");
    fand_observe = 1;
    T += 1000;
    fand_init(&f, &c, T);
    f.on_since_hint = T - 100000;
    run(&f, 3, HOST_ON);
    fand_observe = 0;
    CHECK(reg(2, 0x4c, 0x4a) == 0x38 && reg(2, 0x4c, 0x11) == 0x98 && f.repairs == 0 && f.z[Z_BOARD].failsafe, "observe: nothing written, no data");
}

static void t_fan_range(void)
{
    struct fand f;
    struct config c;
    char e[160];
    cfg_defaults(&c); /* CPU fan 35..65 */
    start(&f, &c);
    run(&f, 300, HOST_ON);
    fand_set_duty(&f, Z_CPU, 30, e, sizeof(e));
    run(&f, 1, HOST_ON);
    CHECK(reg(6, 0x4c, 0x4c) == 16 && NEAR(f.z[Z_CPU].out[0].duty, 35, 0.01), "zone 30%% (FLOOR): CPU fan 35%% = 16 (%d)",
          reg(6, 0x4c, 0x4c));
    fand_set_duty(&f, Z_CPU, 65, e, sizeof(e));
    run(&f, 1, HOST_ON);
    CHECK(reg(6, 0x4c, 0x4c) == 23 && NEAR(f.z[Z_CPU].out[0].duty, 50, 0.01), "zone 65%%: CPU fan 50%% = 23 (%d)", reg(6, 0x4c, 0x4c));
    fand_set_duty(&f, Z_CPU, 99, e, sizeof(e));
    run(&f, 1, HOST_ON);
    CHECK(NEAR(f.z[Z_CPU].out[0].duty, 64.57, 0.01) && reg(6, 0x4c, 0x4c) == 30, "zone 99%%: 64.6%% (%d)", reg(6, 0x4c, 0x4c));
    fand_set_duty(&f, Z_CPU, 100, e, sizeof(e));
    run(&f, 1, HOST_ON);
    CHECK(reg(6, 0x4c, 0x4c) == 46, "zone 100%%: full scale, not 65%% (%d)", reg(6, 0x4c, 0x4c));
    fand_set_duty(&f, Z_BOARD, 30, e, sizeof(e));
    run(&f, 1, HOST_ON);
    CHECK(reg(2, 0x2c, 0x30) == 77 && reg(2, 0x4c, 0x4c) == 14, "board fans: their own range (full): 30%%");
    fand_set_mode(&f, Z_CPU, MODE_AUTO, e, sizeof(e));
    hide(5, 0x21, 1);
    run(&f, 6, HOST_ON);
    CHECK(reg(6, 0x4c, 0x4c) == 46, "fail-safe: full scale");
    hide(5, 0x21, 0);
    fand_failsafe(&c);
    CHECK(reg(6, 0x4c, 0x4c) == 46, "--failsafe: full scale");
    /* config checks */
    struct config k;
    char err[FAND_MAX_ERR][128];
    cfg_defaults(&k);
    int n = cfg_parse(&k, "CPU_FAN_MAX_DUTY=30\n", err, FAND_MAX_ERR);
    CHECK(n == 1 && k.cpu_fan_max == 100, "max not above min/floor: refused, 100 (%d)", n);
    cfg_defaults(&k);
    n = cfg_parse(&k, "CPU_FAN_MIN_DUTY=10\nCASE_FAN_MAX_DUTY=80\nTACH_VALID_DUTY=40\n", err, FAND_MAX_ERR);
    CHECK(n == 1 && k.cpu_fan_min == 35 && k.case_fan_max == 80 && k.tach_valid_duty == 40, "min below 20 refused (%d)", n);
    cfg_defaults(&k);
    k.floor = 40;
    struct fand g;
    fand_init(&g, &k, 0);
    CHECK(NEAR(fan_duty_test(&g.z[Z_CPU].out[0], 40, 40), 40, 0.01), "a FLOOR above the fan minimum wins");
}

static void t_manual(void)
{
    struct fand f;
    struct config c = cfg_test();
    char e[160];
    start(&f, &c);
    run(&f, 300, HOST_ON);
    CHECK(fand_set_duty(&f, Z_CPU, 20, e, sizeof(e)) && f.z[Z_CPU].mode == MODE_AUTO, "below the floor: refused (%s)", e);
    CHECK(fand_set_duty(&f, Z_CPU, 101, e, sizeof(e)), "above 100: refused");
    CHECK(fand_set_duty(&f, Z_CPU, NAN, e, sizeof(e)), "NaN refused");
    CHECK(fand_set_duty(&f, 7, 50, e, sizeof(e)), "no zone 7");
    CHECK(!fand_set_duty(&f, Z_CPU, 70, e, sizeof(e)), "70%%");
    run(&f, 1, HOST_ON);
    CHECK(f.z[Z_CPU].duty == 70 && NEAR(cpu_duty(), 70, 1.1), "manual 70 applied at once (%.1f)", cpu_duty());
    CHECK(!fand_set_duty(&f, Z_CPU, 30, e, sizeof(e)), "30%%");
    run(&f, 1, HOST_ON);
    CHECK(f.z[Z_CPU].duty == 30, "manual 30 at once");
    run(&f, 590, HOST_ON);
    CHECK(f.z[Z_CPU].mode == MODE_MANUAL, "still manual after 591 s");
    CHECK(!fand_confirm(&f, Z_CPU, e, sizeof(e)), "confirm");
    run(&f, 590, HOST_ON);
    CHECK(f.z[Z_CPU].mode == MODE_MANUAL && f.z[Z_CPU].duty == 30, "confirmed: still manual");
    run(&f, 11, HOST_ON);
    CHECK(f.z[Z_CPU].mode == MODE_AUTO, "not confirmed for MANUAL_TIMEOUT_SEC: auto");
    CHECK(fand_confirm(&f, Z_CPU, e, sizeof(e)), "confirm when not manual: error");
    /* manual, but hot */
    fand_set_duty(&f, Z_CPU, 30, e, sizeof(e));
    pcs_temp(0x22, 76);
    run(&f, 2, HOST_ON);
    CHECK(f.z[Z_CPU].duty == 100 && f.z[Z_CPU].hot, "manual, an input at its full-speed point: 100%%");
    pcs_temp(0x22, 52);
    run(&f, 2, HOST_ON);
    CHECK(f.z[Z_CPU].duty == 30, "cool again: the manual duty");
    /* manual, input missing: still fail-safe */
    hide(6, 0x44, 1);
    run(&f, 6, HOST_ON);
    CHECK(f.z[Z_CPU].duty == 100 && f.z[Z_CPU].failsafe, "manual, input missing: 100%%");
    hide(6, 0x44, 0);
    run(&f, 1, HOST_ON);
    /* full */
    CHECK(!fand_set_mode(&f, Z_BOARD, MODE_FULL, e, sizeof(e)), "full");
    run(&f, 1000, HOST_ON);
    CHECK(f.z[Z_BOARD].duty == 100 && f.z[Z_BOARD].mode == MODE_FULL && reg(2, 0x2e, 0x30) == 0xff, "full stays 100%% (no timeout)");
    CHECK(!fand_set_mode(&f, Z_BOARD, MODE_AUTO, e, sizeof(e)), "auto");
    run(&f, 200, HOST_ON);
    CHECK(f.z[Z_BOARD].duty < 100, "auto again");
    CHECK(fand_set_mode(&f, Z_BOARD, 9, e, sizeof(e)), "bad mode refused");
    /* manual from auto keeps the duty it had */
    double d = f.z[Z_BOARD].duty;
    fand_set_mode(&f, Z_BOARD, MODE_MANUAL, e, sizeof(e));
    run(&f, 1, HOST_ON);
    CHECK(NEAR(f.z[Z_BOARD].duty, d, 0.01), "manual: the duty it had (%.1f)", d);
    /* curves */
    CHECK(fand_set_curve(&f, Z_CPU, G_IOHUB, "40:30 70:100", e, sizeof(e)), "cpu has no IOHUB input");
    CHECK(fand_set_curve(&f, Z_CPU, G_PCS, "40:10 70:100", e, sizeof(e)), "curve below the floor refused");
    CHECK(!fand_set_curve(&f, Z_CPU, G_PCS, "40:50 70:100", e, sizeof(e)), "curve set");
    fand_set_mode(&f, Z_CPU, MODE_AUTO, e, sizeof(e));
    run(&f, 200, HOST_ON);
    CHECK(f.z[Z_CPU].duty >= 50, "new curve in use (%.1f)", f.z[Z_CPU].duty);
}

static void t_reload(void)
{
    struct fand f;
    struct config c = cfg_test();
    start(&f, &c);
    run(&f, 300, HOST_ON);
    double d = f.z[Z_CPU].duty;
    /* a sensor that answers only every other poll would blind a zone after
     * a reload that forgot what it had read */
    hide(5, 0x22, 1);
    struct config c2 = c;
    c2.hyst = 2;
    fand_reconfig(&f, &c2);
    run(&f, 1, HOST_ON);
    CHECK(!f.z[Z_CPU].failsafe && NEAR(f.z[Z_CPU].duty, d, 0.01), "reload: sensors' last readings kept (%.1f)", f.z[Z_CPU].duty);
    hide(5, 0x22, 0);
    hide(2, 0x2c, 1);
    fand_reconfig(&f, &c);
    run(&f, 1, HOST_ON);
    CHECK(!f.out_alarm, "reload: fans' state kept, no alarm from a missed write");
    hide(2, 0x2c, 0);
    run(&f, 1, HOST_ON);
    /* a changed address starts from scratch */
    c2 = c;
    c2.pcs[6] = 0x27;
    fand_reconfig(&f, &c2);
    run(&f, 6, HOST_ON);
    CHECK(f.z[Z_CPU].failsafe, "reload: a new (missing) sensor address: fail-safe");
    fand_reconfig(&f, &c);
    run(&f, 1, HOST_ON);
    CHECK(!f.z[Z_CPU].failsafe, "back");
}

/* an observed Off: what the run file said about the host is over */
static void t_on_since_hint(void)
{
    struct fand f;
    struct config c = cfg_test();
    board();
    T = 1000000;
    fand_init(&f, &c, T);
    f.on_since_hint = T - 100000;
    run(&f, 5, HOST_OFF);
    run(&f, 1, HOST_ON);
    CHECK(f.phase == PH_SETTLE, "the host seen off, then on: it settles (phase %d)", f.phase);

    fand_init(&f, &c, T);
    f.on_since_hint = T - 100000;
    run(&f, 1, HOST_PENDING);
    run(&f, 1, HOST_ON);
    CHECK(f.phase == PH_RUN, "not known yet, then on: the hint counts (phase %d)", f.phase);
}

/* the EMC2305 watchdog fires at 4 s */
static void t_poll_watchdog(void)
{
    struct config c;
    char err[FAND_MAX_ERR][128];
    cfg_defaults(&c);
    int n = cfg_parse(&c, "POLL_MS=5000\n", err, FAND_MAX_ERR);
    CHECK(n >= 1 && c.poll_ms == 3000 && strstr(err[0], "POLL_MS"), "POLL_MS over 3 s with the watchdog: an error, 3000 (%d, %d)", n, c.poll_ms);
    CHECK(c.stale_ms >= 2 * c.poll_ms, "... the stale rule holds (%d)", c.stale_ms);
    cfg_defaults(&c);
    n = cfg_parse(&c, "POLL_MS=3001\nSTALE_SEC=10\n", err, FAND_MAX_ERR);
    CHECK(n == 1 && c.poll_ms == 3000 && c.stale_ms == 10000, "3001: 3000 (%d, %d)", n, c.poll_ms);
    cfg_defaults(&c);
    n = cfg_parse(&c, "POLL_MS=3000\nSTALE_SEC=10\n", err, FAND_MAX_ERR);
    CHECK(n == 0 && c.poll_ms == 3000, "3000 is fine (%d)", n);
    cfg_defaults(&c);
    n = cfg_parse(&c, "POLL_MS=5000\nSTALE_SEC=10\nEMC_WATCHDOG=no\n", err, FAND_MAX_ERR);
    CHECK(n == 0 && c.poll_ms == 5000, "no watchdog: 5000 stays (%d, %d)", n, c.poll_ms);
}

/* run fand_failsafe, what it logged in *log */
static int64_t failsafe_logged(const struct config *c, char *log, size_t size)
{
    char file[300];
    snprintf(file, sizeof(file), "%s.log", R);
    fflush(stderr);
    int save = dup(2), fd = open(file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || dup2(fd, 2) < 0) abort();
    close(fd);
    int q = fand_quiet;
    fand_quiet = 0;
    int64_t t0 = s_hw_ms;
    fand_failsafe(c);
    int64_t took = s_hw_ms - t0;
    fand_quiet = q;
    fflush(stderr);
    dup2(save, 2);
    close(save);
    fd = open(file, O_RDONLY);
    ssize_t n = fd < 0 ? -1 : read(fd, log, size - 1);
    log[n > 0 ? n : 0] = 0;
    if (fd >= 0) close(fd);
    unlink(file);
    return took;
}

static void t_failsafe_verify(void)
{
    struct config c = cfg_test();
    char log[2000];

    /* the corrupted state seen on the board, the write of 0x4A refused once */
    board();
    no_faults();
    setreg(2, 0x4c, 0x4a, 0x3b);
    I.bus = 2, I.addr = 0x4c, I.reg = 0x4a, I.nak = 1;
    failsafe_logged(&c, log, sizeof(log));
    CHECK(reg(2, 0x4c, 0x4a) == 0x2b && reg(2, 0x4c, 0x4c) == 46, "0x4A refused once: retried (0x%02x, %d)", reg(2, 0x4c, 0x4a), reg(2, 0x4c, 0x4c));

    /* acknowledged and lost, twice */
    board();
    no_faults();
    setreg(2, 0x4c, 0x4a, 0x3b);
    I.bus = 2, I.addr = 0x4c, I.reg = 0x4a, I.lose = 2;
    failsafe_logged(&c, log, sizeof(log));
    CHECK(reg(2, 0x4c, 0x4a) == 0x2b && reg(2, 0x4c, 0x4c) == 46, "0x4A lost twice: read back, retried (0x%02x, %d)", reg(2, 0x4c, 0x4a), reg(2, 0x4c, 0x4c));

    /* the duty itself */
    board();
    no_faults();
    setreg(6, 0x4c, 0x4c, 5);
    I.bus = 6, I.addr = 0x4c, I.reg = 0x4c, I.lose = 2;
    failsafe_logged(&c, log, sizeof(log));
    CHECK(reg(6, 0x4c, 0x4c) == 46, "0x4C lost twice: retried (%d)", reg(6, 0x4c, 0x4c));
    CHECK(!strstr(log, "<3>"), "... nothing to report: %s", log);

    /* an EMC2305 channel */
    board();
    no_faults();
    setreg(2, 0x2c, 0x50, 5);
    I.bus = 2, I.addr = 0x2c, I.reg = 0x50, I.nak = 2;
    failsafe_logged(&c, log, sizeof(log));
    CHECK(all_full(), "an EMC2305 channel refused twice: retried (%d)", reg(2, 0x2c, 0x50));

    /* never takes it: said, the others done */
    board();
    no_faults();
    I.bus = 2, I.addr = 0x2c, I.reg = 0x40, I.nak = 1000;
    failsafe_logged(&c, log, sizeof(log));
    CHECK(strstr(log, "<3>") && reg(2, 0x2c, 0x30) == 0xff && reg(2, 0x2c, 0x50) == 0xff && reg(2, 0x2e, 0x40) == 0xff,
          "an EMC2305 channel refused for good: an error, the rest 100%% (%s)", log);

    /* the polarity bit can't be cleared: inverted, full scale is 0 */
    board();
    no_faults();
    setreg(2, 0x4c, 0x4a, 0x3b);
    setreg(2, 0x4c, 0x4c, 5);
    I.bus = 2, I.addr = 0x4c, I.reg = 0x4a, I.stick = 0x10;
    failsafe_logged(&c, log, sizeof(log));
    CHECK((reg(2, 0x4c, 0x4a) & 0x10) && reg(2, 0x4c, 0x4c) == 0, "polarity stuck: 0x4C 0, full with it inverted (%d)", reg(2, 0x4c, 0x4c));
    CHECK(strstr(log, "<3>") != NULL, "... said at error level: %s", log);

    /* ... but only when it was seen: written, then 0x4A unreadable must not stop the fan */
    board();
    no_faults();
    setreg(2, 0x4c, 0x4a, 0x3b);
    I.bus = 2, I.addr = 0x4c, I.reg = 0x4a, I.rd_fail_from = 1;
    failsafe_logged(&c, log, sizeof(log));
    CHECK(reg(2, 0x4c, 0x4a) == 0x2b && reg(2, 0x4c, 0x4c) != 0, "cleared, not read back: no 0 written (%d)", reg(2, 0x4c, 0x4c));

    /* PWPGM can't be set: the chip runs its lookup table */
    board();
    no_faults();
    setreg(2, 0x4c, 0x4a, 0x0b);
    I.bus = 2, I.addr = 0x4c, I.reg = 0x4a, I.lock = 1;
    failsafe_logged(&c, log, sizeof(log));
    CHECK(strstr(log, "<3>") && strstr(log, "PWPGM"), "PWPGM not set: said at error level: %s", log);

    /* time: a dead bus doesn't keep the others out, and the unit's TimeoutStopSec=15 */
    board();
    no_faults();
    setreg(2, 0x4c, 0x4c, 5);
    setreg(2, 0x2c, 0x30, 5);
    setreg(2, 0x2e, 0x30, 5);
    I.dead_bus = 6;
    int64_t took = failsafe_logged(&c, log, sizeof(log));
    CHECK(reg(2, 0x4c, 0x4c) == 46 && reg(2, 0x2c, 0x30) == 0xff && reg(2, 0x2e, 0x30) == 0xff && took <= 3000,
          "the CPU fan's bus dead: the others 100%%, %lld ms", (long long)took);

    board();
    no_faults();
    I.dead_bus = -2;
    took = failsafe_logged(&c, log, sizeof(log));
    CHECK(took <= 7500, "every bus dead: %lld ms", (long long)took);

    board();
    no_faults();
    setreg(6, 0x4c, 0x4a, 0x3b);
    setreg(2, 0x4c, 0x4a, 0x3b);
    I.dead_wr_bus = 2;
    took = failsafe_logged(&c, log, sizeof(log));
    CHECK(took <= 9000 && reg(6, 0x4c, 0x4c) == 46, "every write on bus 2 hanging: %lld ms, the CPU fan done", (long long)took);

    /* the worst there is: each chip answers its ID, then each transfer hangs */
    board();
    no_faults();
    setreg(6, 0x4c, 0x4a, 0x3b);
    setreg(2, 0x4c, 0x4a, 0x3b);
    I.hang_rest = 1;
    took = failsafe_logged(&c, log, sizeof(log));
    CHECK(took <= 10500, "every transfer hanging after the ID: %lld ms (TimeoutStopSec=15)", (long long)took);
    no_faults();
}

static void t_json(void)
{
    struct fand f;
    struct config c = cfg_test();
    start(&f, &c);
    run(&f, 30, HOST_ON);
    char e[160];
    fand_set_duty(&f, Z_CPU, 55, e, sizeof(e));
    hide(5, 0x21, 1);
    run(&f, 6, HOST_ON);
    for (int k = 0; k < 2; k++)
    {
        struct buf b = { 0 };
        fand_json(&f, &b);
        char file[300], cmd[700];
        snprintf(file, sizeof(file), "%s.json", R);
        FILE *fp = fopen(file, "w");
        fputs(b.s, fp);
        fclose(fp);
        snprintf(cmd, sizeof(cmd), "python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); "
                 "assert d[\"zones\"][0][\"id\"]==\"cpu\" and len(d[\"inputs\"])==4' %s", file);
        CHECK(!b.oom && system(cmd) == 0, "JSON parses (%s)", k ? "host off" : "running, alarm");
        CHECK(strstr(b.s, "nan") == NULL, "no nan in JSON");
        free(b.s);
        unlink(file);
        run(&f, 2, HOST_OFF);
    }
}

/* The bus the platform shares (2: the IOHUB chip and both EMC2305): steady
 * fans, so the EMC2305 drive isn't written again every poll (5 writes and 5
 * reads per chip): when it changes, else every EMC_KEEPALIVE_MS (its
 * watchdog fires at 4 s), one register read back in between */
static void t_bus_cost(void)
{
    struct fand f;
    struct config c = cfg_test();
    start(&f, &c);
    run(&f, 300, HOST_ON); /* steady */
    unsigned long x0 = s_bus_xfers[2], w0 = s_emc_drive_wr;
    s_emc_max_gap = 0;
    s_emc_last_wr[0] = s_emc_last_wr[1] = 0;
    run(&f, 20, HOST_ON);
    unsigned long per_s = (s_bus_xfers[2] - x0) / 20, wr = s_emc_drive_wr - w0;
    CHECK(per_s <= 41, "bus 2: %lu transfers a second (50 when every poll wrote the EMC2305: 43, and 7 of the readings for the others)", per_s);
    CHECK(wr <= 2 * 5 * 10, "EMC2305 drive written %lu times in 20 s (keep-alive every 2 s: 100)", wr);
    CHECK(s_emc_max_gap > 0 && s_emc_max_gap <= 2000, "the watchdog fed every %lld ms at most (fires at 4 s)", (long long)s_emc_max_gap);
    CHECK(!fand_alarm(&f) && f.z[Z_BOARD].out[1].ok && f.z[Z_BOARD].out[2].ok, "no alarm, the EMC2305 driven");
    /* a drive register changed behind our back: back within the keep-alive */
    int want = reg(2, 0x2c, 0x40);
    setreg(2, 0x2c, 0x40, 0x00);
    run(&f, 2, HOST_ON);
    CHECK(reg(2, 0x2c, 0x40) == want, "fan 1 of 2-002c put back (0x%02x, 0x%02x)", reg(2, 0x2c, 0x40), want);
    /* the first channel read back every poll: a change there at once */
    setreg(2, 0x2e, 0x30, 0x00);
    run(&f, 1, HOST_ON);
    CHECK(reg(2, 0x2e, 0x30) == want, "fan 0 of 2-002e put back in one poll (0x%02x)", reg(2, 0x2e, 0x30));
    /* a duty change is written at once */
    f.z[Z_BOARD].mode = MODE_FULL;
    run(&f, 1, HOST_ON);
    CHECK(reg(2, 0x2c, 0x70) == 0xff && reg(2, 0x2e, 0x70) == 0xff, "full: written in that poll");
    /* a chip gone: alarmed as before (STALE_SEC, not sooner, not later) */
    f.z[Z_BOARD].mode = MODE_AUTO;
    run(&f, 300, HOST_ON);
    hide(2, 0x2e, 1);
    run(&f, 5, HOST_ON);
    CHECK(!f.out_alarm, "gone 5 s: not yet");
    run(&f, 2, HOST_ON);
    CHECK(f.out_alarm, "gone 7 s: alarm");
    hide(2, 0x2e, 0);
    /* a long poll (3 s, the most with the watchdog): every poll writes */
    struct config c3 = c;
    c3.poll_ms = 3000;
    fand_reconfig(&f, &c3);
    run(&f, 30, HOST_ON);
    w0 = s_emc_drive_wr;
    run(&f, 30, HOST_ON);
    CHECK(s_emc_drive_wr - w0 == 10 * 10, "POLL_MS 3000: written every poll (%lu in 10 polls)", s_emc_drive_wr - w0);
}

static const struct fand_reading *reading(const struct fand_reading *r, int n, const char *dev, const char *input)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(r[i].dev, dev) && !strcmp(r[i].input, input)) return &r[i];
    return NULL;
}

/* EMC2305 fan n's tachometer for an rpm (RANGE m = 2, as the board sets it) */
static void emc_rpm(int addr, int n, double rpm)
{
    int raw = rpm <= 0 ? 0xfff8 : ((int)lround(3932160.0 * 2 / rpm) << 3) & 0xfff8;
    setreg(2, addr, 0x3e + 0x10 * n, raw >> 8);
    setreg(2, addr, 0x3f + 0x10 * n, raw & 0xff);
}

/* What reimu-fand read, given to the others (D-Bus Readings: reimu-hwmon
 * publishes it for Redfish, IPMI and the web UI), so no one else reads these
 * chips: its control inputs as it has them, and the rest of its chips'
 * inputs (local temperatures, every tachometer) every EXTRAS_MS */
static void t_readings(void)
{
    struct fand f;
    struct config c = cfg_test();
    struct fand_reading r[FAND_MAX_READINGS];
    board();
    setreg(6, 0x4c, 0x00, 40);
    setreg(2, 0x4c, 0x00, 45);
    setreg(2, 0x4c, 0x46, 0x10);
    setreg(2, 0x4c, 0x47, 0x0e); /* 0x0e10: 1500 rpm */
    emc_rpm(0x2c, 0, 3000);
    emc_rpm(0x2e, 4, 1200);
    T = 1000000;
    fand_init(&f, &c, T);
    CHECK(fand_readings(&f, r, FAND_MAX_READINGS) == 0, "not controlling: no readings");
    f.on_since_hint = T - 100000;
    run(&f, c.start_full_ms / 1000.0 + 6, HOST_ON);
    int n = fand_readings(&f, r, FAND_MAX_READINGS);
    const struct fand_reading *x;
    for (int a = 0x20; a <= 0x26; a++)
    {
        char d[12];
        snprintf(d, sizeof(d), "5-%04x", a);
        x = reading(r, n, d, "temp1");
        CHECK(x && NEAR(x->value, 50 + (a - 0x20), 0.01) && isnan(x->duty) && x->at == T, "PCS %s temp1 %.2f", d,
              x ? x->value : -1);
    }
    x = reading(r, n, "6-0045", "temp1");
    CHECK(x && NEAR(x->value, 65, 0.01), "VRM 6-0045 temp1 (%.2f)", x ? x->value : -1);
    x = reading(r, n, "2-004c", "temp2");
    CHECK(x && NEAR(x->value, 83, 0.01) && x->at == T, "IOHUB: 2-004c temp2 (%.2f)", x ? x->value : -1);
    x = reading(r, n, "6-004c", "temp1");
    CHECK(x && NEAR(x->value, 40, 0.01) && x->at > 0, "local: 6-004c temp1 (%.2f)", x ? x->value : -1);
    x = reading(r, n, "2-004c", "temp1");
    CHECK(x && NEAR(x->value, 45, 0.01), "local: 2-004c temp1 (%.2f)", x ? x->value : -1);
    CHECK(!reading(r, n, "6-004c", "temp2"), "the CPU fan chip's remote diode: not connected, not given");
    x = reading(r, n, "6-004c", "fan1");
    CHECK(x && NEAR(x->value, 2000, 5) && NEAR(x->duty, f.z[Z_CPU].out[0].duty, 0.01) && x->at == T, "CPU fan: %.0f rpm at %.0f%%",
          x ? x->value : -1, x ? x->duty : -1);
    x = reading(r, n, "2-004c", "fan1");
    CHECK(x && NEAR(x->value, 1500, 1) && NEAR(x->duty, f.z[Z_BOARD].out[0].duty, 0.01), "board fan: %.0f rpm", x ? x->value : -1);
    x = reading(r, n, "2-002c", "fan1");
    CHECK(x && NEAR(x->value, 3000, 2) && NEAR(x->duty, f.z[Z_BOARD].out[1].duty, 0.01), "case fan 2-002c fan1: %.0f rpm",
          x ? x->value : -1);
    x = reading(r, n, "2-002e", "fan5");
    CHECK(x && NEAR(x->value, 1200, 2), "case fan 2-002e fan5: %.0f rpm", x ? x->value : -1);
    x = reading(r, n, "2-002c", "fan2");
    CHECK(x && isnan(x->value) && !isnan(x->duty), "no fan on 2-002c fan2: no rpm, still its drive");
    CHECK(n == 7 + 3 + 1 + 2 + 2 + 10, "%d readings", n);

    /* the extras: every 5 s, not every poll */
    unsigned long x0 = s_emc_tach_rd;
    run(&f, 10, HOST_ON);
    CHECK(s_emc_tach_rd - x0 == 2, "the extras twice in 10 s (%lu)", s_emc_tach_rd - x0);
    x = reading(r, fand_readings(&f, r, FAND_MAX_READINGS), "6-004c", "temp1");
    CHECK(x && T - x->at <= 5000, "and fresh (%lld ms)", x ? (long long)(T - x->at) : -1LL);

    /* a PCS sensor that stopped answering: no value once stale */
    hide(5, 0x23, 1);
    run(&f, 1, HOST_ON);
    n = fand_readings(&f, r, FAND_MAX_READINGS);
    x = reading(r, n, "5-0023", "temp1");
    CHECK(x && !isnan(x->value) && x->at < T, "5-0023 not read: its last value while fresh");
    run(&f, c.stale_ms / 1000.0 + 1, HOST_ON);
    n = fand_readings(&f, r, FAND_MAX_READINGS);
    x = reading(r, n, "5-0023", "temp1");
    CHECK(x && isnan(x->value), "stale: no value (%.2f)", x ? x->value : -1);
    hide(5, 0x23, 0);
    /* a local temperature that stopped answering: none after 3 passes */
    hide(6, 0x4c, 1);
    run(&f, 16, HOST_ON);
    n = fand_readings(&f, r, FAND_MAX_READINGS);
    x = reading(r, n, "6-004c", "temp1");
    CHECK(x && isnan(x->value), "6-004c gone: its local temperature none (%.2f)", x ? x->value : -1);
    hide(6, 0x4c, 0);
    run(&f, 2, HOST_OFF);
    CHECK(fand_readings(&f, r, FAND_MAX_READINGS) == 0, "host off: none");
}

/* ------------------------------------------------------------------ hold a temperature */
/* a CPU that settles at EQ0 - 0.375 * fan duty (80 % -> 50 °C at EQ0 80, as
 * measured on the board), time constant 20 s; every PCS sensor at it */
static double s_cpu_t;
static void run_model(struct fand *f, double sec, double eq0, double *tmin, double *tmax, double *dmin, double *dmax)
{
    for (int i = 0; i < (int)(sec * 1000 / f->c.poll_ms + 0.5); i++)
    {
        double eq = eq0 - 0.375 * f->z[Z_CPU].duty;
        s_cpu_t += (eq - s_cpu_t) * (f->c.poll_ms / 1000.0) / 20;
        for (int a = 0x20; a <= 0x26; a++) pcs_temp(a, s_cpu_t);
        T += f->c.poll_ms;
        fand_step(f, T, HOST_ON);
        if (tmin && s_cpu_t < *tmin) *tmin = s_cpu_t;
        if (tmax && s_cpu_t > *tmax) *tmax = s_cpu_t;
        if (dmin && f->z[Z_CPU].duty < *dmin) *dmin = f->z[Z_CPU].duty;
        if (dmax && f->z[Z_CPU].duty > *dmax) *dmax = f->z[Z_CPU].duty;
    }
}

static void t_target(void)
{
    struct config c;
    char err[FAND_MAX_ERR][128], e[160];
    /* config */
    cfg_defaults(&c);
    CHECK(isnan(c.tgt_lo[Z_CPU][G_PCS]) && isnan(c.tgt_hi[Z_BOARD][G_IOHUB]), "no target by default");
    int n = cfg_parse(&c, "CPU_PCS_TARGET=48-52\nBOARD_PCS_TARGET=\"50\"\nBOARD_GPU_TARGET=60-70\n", err, FAND_MAX_ERR);
    CHECK(n == 0 && c.tgt_lo[Z_CPU][G_PCS] == 48 && c.tgt_hi[Z_CPU][G_PCS] == 52, "48-52 (%d: %s)", n, n ? err[0] : "");
    CHECK(c.tgt_lo[Z_BOARD][G_PCS] == 50 && c.tgt_hi[Z_BOARD][G_PCS] == 50, "one temperature: lo = hi");
    CHECK(c.tgt_lo[Z_BOARD][G_GPU] == 60, "every curve input can have one");
    n = cfg_parse(&c, "CPU_PCS_TARGET=none\n", err, FAND_MAX_ERR);
    CHECK(n == 0 && isnan(c.tgt_lo[Z_CPU][G_PCS]), "none: off");
    const char *bad[] = { "CPU_PCS_TARGET=52-48", "CPU_PCS_TARGET=20-25", "CPU_PCS_TARGET=60-80", "CPU_PCS_TARGET=75",
                          "CPU_PCS_TARGET=abc", "CPU_PCS_TARGET=30-60", "CPU_PCS_TARGET=50-", "CPU_IOHUB_TARGET=50" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
    {
        cfg_defaults(&c);
        n = cfg_parse(&c, bad[i], err, FAND_MAX_ERR);
        CHECK(n == 1 && isnan(c.tgt_lo[Z_CPU][G_PCS]), "refused: %s (%d)", bad[i], n);
    }
    cfg_defaults(&c);
    n = cfg_parse(&c, "CPU_PCS_TARGET=60-65\nCPU_PCS_CURVE=40:30 62:100\n", err, FAND_MAX_ERR);
    CHECK(n == 1 && isnan(c.tgt_lo[Z_CPU][G_PCS]), "checked against the same file's curve, after it: below its full-speed point (%d)", n);
    CHECK(target_key(Z_CPU, G_PCS) && !strcmp(target_key(Z_CPU, G_PCS), "CPU_PCS_TARGET") && !target_key(Z_CPU, G_IOHUB), "keys");

    /* the loop: from 60 °C into 48-52 and held there, no swinging */
    struct fand f;
    c = cfg_test();
    cfg_parse(&c, "CPU_PCS_TARGET=48-52\n", err, FAND_MAX_ERR);
    start(&f, &c);
    s_cpu_t = 60;
    run_model(&f, 300, 80, NULL, NULL, NULL, NULL);
    double tmin = 1e9, tmax = -1e9, dmin = 1e9, dmax = -1e9;
    run_model(&f, 600, 80, &tmin, &tmax, &dmin, &dmax);
    CHECK(tmin >= 47.5 && tmax <= 52.5, "held in 48-52 (%.2f..%.2f)", tmin, tmax);
    CHECK(dmax - dmin < 10 && dmax < 100, "the duty steady, not at full (%.1f..%.1f)", dmin, dmax);
    CHECK(!f.z[Z_CPU].hot && !f.z[Z_CPU].failsafe, "neither hot nor fail-safe");
    /* more load: the duty goes up, still in the band */
    run_model(&f, 600, 85, NULL, NULL, NULL, NULL);
    tmin = 1e9, tmax = -1e9, dmin = 1e9, dmax = -1e9;
    run_model(&f, 300, 85, &tmin, &tmax, &dmin, &dmax);
    CHECK(tmin >= 47.5 && tmax <= 52.5 && dmin > 80, "more load: more duty, still held (%.2f..%.2f, %.1f%%)", tmin, tmax, dmin);
    /* idle, cool below the band at any duty: down to the floor */
    run_model(&f, 900, 55, NULL, NULL, NULL, NULL);
    CHECK(NEAR(f.z[Z_CPU].duty, c.floor, 0.01), "below the band: down to the floor (%.1f)", f.z[Z_CPU].duty);
    /* safety: at the curve's full-speed point 100 % (at SLEW_UP, as the curves) */
    for (int a = 0x20; a <= 0x26; a++) pcs_temp(a, 76);
    run(&f, 8, HOST_ON);
    CHECK(f.z[Z_CPU].duty == 100 && f.z[Z_CPU].hot && f.z[Z_CPU].in[G_PCS].demand == 100, "76 °C: 100%% (%.1f)", f.z[Z_CPU].duty);
    /* JSON shows it */
    struct buf b = { 0 };
    fand_json(&f, &b);
    CHECK(b.s && strstr(b.s, "\"target\":[48,52]") && strstr(b.s, "\"target\":null"), "JSON: target [48,52], others null");
    free(b.s);

    /* set over D-Bus */
    CHECK(fand_set_target(&f, Z_CPU, G_PCS, "60-80", e, sizeof(e)), "above the curve's full speed refused");
    CHECK(fand_set_target(&f, Z_CPU, G_IOHUB, "50", e, sizeof(e)), "cpu has no IOHUB input");
    CHECK(!fand_set_target(&f, Z_CPU, G_PCS, "none", e, sizeof(e)) && isnan(f.c.tgt_lo[Z_CPU][G_PCS]), "none: off");
    for (int a = 0x20; a <= 0x26; a++) pcs_temp(a, 60);
    run(&f, 300, HOST_ON);
    CHECK(NEAR(f.z[Z_CPU].in[G_PCS].demand, 47, 0.01), "off: the curve again (%.2f)", f.z[Z_CPU].in[G_PCS].demand);
    CHECK(!fand_set_target(&f, Z_CPU, G_PCS, "48-52", e, sizeof(e)) && f.c.tgt_hi[Z_CPU][G_PCS] == 52, "set (%s)", e);
    double d0 = f.z[Z_CPU].duty;
    run(&f, 1, HOST_ON);
    CHECK(f.z[Z_CPU].duty >= d0 - 1.01, "turned on: starts from the curve's duty, no drop (%.1f -> %.1f)", d0, f.z[Z_CPU].duty);
}

int main(void)
{
    snprintf(R, sizeof(R), "%s/fand-test-%d", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", getpid());
    setenv("REIMU_FAND_FAKE", R, 1);
    no_faults();
    fand_quiet = getenv("VERBOSE") == NULL;
    struct { const char *name; void (*fn)(void); } tests[] = {
        { "curves", t_curves }, { "config", t_config }, { "host", t_host }, { "hysteresis_slew", t_hysteresis_slew },
        { "failsafe_inputs", t_failsafe_inputs }, { "outputs", t_outputs }, { "tach", t_tach }, { "manual", t_manual },
        { "reload", t_reload }, { "json", t_json }, { "fan_model", t_fan_model }, { "fan_range", t_fan_range },
        { "tach_mode", t_tach_mode }, { "floor_full", t_floor_full }, { "turbo", t_turbo }, { "gpu", t_gpu }, { "guard", t_guard },
        { "on_since_hint", t_on_since_hint }, { "poll_watchdog", t_poll_watchdog }, { "failsafe_verify", t_failsafe_verify }, { "bus_cost", t_bus_cost }, { "readings", t_readings }, { "target", t_target },
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++)
    {
        int f0 = s_fail;
        board();
        tests[i].fn();
        printf("%-18s %s\n", tests[i].name, s_fail == f0 ? "ok" : "FAILED");
    }
    char cmd[300];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", R);
    if (system(cmd)) return 2;
    printf("%d checks, %d failed\n", s_pass + s_fail, s_fail);
    return s_fail ? 1 : 0;
}
