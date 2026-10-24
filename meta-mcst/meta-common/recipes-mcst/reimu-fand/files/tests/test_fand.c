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

static int s_fail, s_pass;
static char R[200]; /* the fake root */
static int64_t T;   /* simulated ms */

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
    r[0x4a] = 0x2b;
    r[0x4b] = 0x3f;
    r[0x4c] = 0x2e;
    r[0x4d] = 0x17;
    r[0x4f] = 0x04;
    r[0x50] = 0x7f;
    r[0x51] = 0x3f;
    r[0x03] = 0x06;
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
    /* PWM_FREQ changed under it: the scale follows */
    setreg(6, 0x4c, 0x4d, 0x10);
    run(&f, 1, HOST_ON);
    CHECK(reg(6, 0x4c, 0x4c) == (int)lround(f.z[Z_CPU].duty * 32 / 100), "PWM_FREQ 0x10: %d of 32", reg(6, 0x4c, 0x4c));
    setreg(6, 0x4c, 0x4d, 0x17);
    /* the high-resolution mode: 8 bits, 255 = 100 % */
    setreg(6, 0x4c, 0x45, 0x10);
    setreg(6, 0x4c, 0x4a, 0x23);
    setreg(6, 0x4c, 0x4d, 0x08);
    run(&f, 1, HOST_ON);
    CHECK(reg(6, 0x4c, 0x4c) == (int)lround(f.z[Z_CPU].duty * 255 / 100) && f.z[Z_CPU].out[0].ok, "high resolution: %d of 255",
          reg(6, 0x4c, 0x4c));
    fand_failsafe(&c);
    CHECK(reg(6, 0x4c, 0x4c) == 255, "fail-safe, high resolution: 255");
    setreg(6, 0x4c, 0x45, 0x00);
    setreg(6, 0x4c, 0x4a, 0x2b);
    setreg(6, 0x4c, 0x4d, 0x17);
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
    /* the 1.4 kHz clock bit and polarity kept, whatever they are */
    setreg(6, 0x4c, 0x4a, 0x33);
    run(&f, 1, HOST_ON);
    CHECK(reg(6, 0x4c, 0x4a) == 0x32, "bits 3, 4 kept (0x%02x)", reg(6, 0x4c, 0x4a));
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

int main(void)
{
    snprintf(R, sizeof(R), "%s/fand-test-%d", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", getpid());
    setenv("REIMU_FAND_FAKE", R, 1);
    fand_quiet = getenv("VERBOSE") == NULL;
    struct { const char *name; void (*fn)(void); } tests[] = {
        { "curves", t_curves }, { "config", t_config }, { "host", t_host }, { "hysteresis_slew", t_hysteresis_slew },
        { "failsafe_inputs", t_failsafe_inputs }, { "outputs", t_outputs }, { "tach", t_tach }, { "manual", t_manual },
        { "reload", t_reload }, { "json", t_json }, { "fan_model", t_fan_model }, { "fan_range", t_fan_range },
        { "tach_mode", t_tach_mode }, { "floor_full", t_floor_full }, { "turbo", t_turbo }, { "gpu", t_gpu },
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
