/* reimu-fand control logic: config, curves, zones, fail-safe, alarms.
 *
 * Two zones, each driven by the hottest demand of its inputs:
 *   cpu    the CPU fan (X37, lm96163 on BMC i2c-6 0x4c)
 *          inputs: E8C2 PCS (max of its sensors), the VRMs (ir38064)
 *   board  X38/X39/X40 (lm96163 on i2c-2 0x4c) and the case fans (both
 *          EMC2305 on i2c-2 0x2c, 0x2e, 5 channels each)
 *          inputs: IOHUB (remote diode of that lm96163), PCS, VRMs
 * An input's demand is its curve (duty against temperature, linear between
 * points) at its temperature after hysteresis: the duty only falls once the
 * temperature fell HYSTERESIS below the one that set it. The duty moves to
 * the demand at most SLEW_UP / SLEW_DOWN %/s and never goes below FLOOR.
 *
 * Fail-safe, all of them 100 % at once (no slew):
 *   - a sensor of an input not read for STALE_SEC: that input's zones
 *   - a fan chip not written (and read back) for STALE_SEC, or not the chip
 *     it should be, or the CPU fan's tachometer below TACH_MIN_RPM for
 *     TACH_ALARM_SEC: every zone, the fan fault LED
 *   - for START_FULL_SEC after taking control (daemon start, host power-on)
 *   - mode "full"; in manual mode, an input at its curve's full-speed point
 *   - the daemon exits or dies: fand_failsafe() (also ExecStopPost)
 * With the host off its devices are unpowered: no I2C at all. After it
 * powers on, the bus is left alone for SETTLE_SEC (its boot uses it).
 *
 * Copyright (c) 2026 MCST REIMU contributors. Licensed under GPLv2.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "fand.h"

#define LED_NAME "platform:red:fanfail"
#define ABS_MIN_FLOOR 20.0 /* no config can make it lower */
#define TEMP_MIN 5.0       /* below: not a reading of a running part */
#define TEMP_MAX 150.0

const char *const group_id[NGROUPS] = { "pcs", "vrm", "iohub", "gpu" };
static const char *const group_name[NGROUPS] = { "Процессор (PCS)", "VRM", "IOHUB", "GPU (агент хоста)" };
const char *const zone_id[NZONES] = { "cpu", "board" };
static const char *const zone_name[NZONES] = { "Процессор (X37)", "Плата и корпус (X38–X40, EMC2305)" };
const char *const mode_name[3] = { "auto", "manual", "full" };

int fand_quiet;
int fand_observe;

void fand_log(int prio, const char *fmt, ...)
{
    if (fand_quiet) return;
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    fprintf(stderr, "<%d>%s\n", prio, line);
}

/* ------------------------------------------------------------------ buf */
void buf_printf(struct buf *b, const char *fmt, ...)
{
    if (b->oom) return;
    for (;;)
    {
        va_list ap;
        va_start(ap, fmt);
        size_t room = b->cap - b->len;
        int n = vsnprintf(b->s ? b->s + b->len : NULL, b->s ? room : 0, fmt, ap);
        va_end(ap);
        if (n < 0)
        {
            b->oom = 1;
            return;
        }
        if (b->s && (size_t)n < room)
        {
            b->len += n;
            return;
        }
        size_t cap = b->cap ? b->cap * 2 : 4096;
        while (cap < b->len + n + 1) cap *= 2;
        char *s = realloc(b->s, cap);
        if (!s)
        {
            b->oom = 1;
            return;
        }
        b->s = s;
        b->cap = cap;
    }
}

static void buf_jstr(struct buf *b, const char *s)
{
    buf_printf(b, "\"");
    for (; *s; s++)
    {
        unsigned char c = *s;
        if (c == '"' || c == '\\') buf_printf(b, "\\%c", c);
        else if (c < 0x20) buf_printf(b, "\\u%04x", c);
        else buf_printf(b, "%c", c);
    }
    buf_printf(b, "\"");
}

static void buf_num(struct buf *b, double v, int digits)
{
    if (isnan(v) || isinf(v)) buf_printf(b, "null");
    else buf_printf(b, "%.*f", digits, v);
}

/* ------------------------------------------------------------------ curves */
double curve_eval(const struct curve *cv, double t)
{
    if (cv->n < 1 || isnan(t)) return 100;
    if (t <= cv->t[0]) return cv->d[0];
    for (int i = 1; i < cv->n; i++)
        if (t < cv->t[i])
            return cv->d[i - 1] + (cv->d[i] - cv->d[i - 1]) * (t - cv->t[i - 1]) / (cv->t[i] - cv->t[i - 1]);
    return cv->d[cv->n - 1];
}

void curve_format(const struct curve *cv, char *buf, size_t n)
{
    size_t len = 0;
    buf[0] = 0;
    for (int i = 0; i < cv->n && len < n; i++)
        len += snprintf(buf + len, n - len, "%s%g:%g", i ? " " : "", cv->t[i], cv->d[i]);
}

/* "T:D T:D ...": 2..8 points, temperatures rising (0..125 °C), duties not
 * falling, all within [floor, 100], the last one 100: whatever the
 * temperature, the curve gives at least the floor, and full speed when hot */
int curve_parse(const char *s, struct curve *cv, double floor, char *err, size_t n)
{
    struct curve c = { 0 };
    const char *p = s;
    while (*p)
    {
        while (*p == ' ' || *p == '\t' || *p == ',' || *p == ';') p++;
        if (!*p) break;
        if (c.n == FAND_MAX_POINTS)
        {
            snprintf(err, n, "more than %d points", FAND_MAX_POINTS);
            return -1;
        }
        char *e;
        double t = strtod(p, &e);
        if (e == p || *e != ':')
        {
            snprintf(err, n, "point %d: not TEMP:DUTY", c.n + 1);
            return -1;
        }
        p = e + 1;
        double d = strtod(p, &e);
        if (e == p || (*e && *e != ' ' && *e != '\t' && *e != ',' && *e != ';'))
        {
            snprintf(err, n, "point %d: not TEMP:DUTY", c.n + 1);
            return -1;
        }
        p = e;
        if (!(t >= 0 && t <= 125))
        {
            snprintf(err, n, "point %d: temperature %g outside 0..125 °C", c.n + 1, t);
            return -1;
        }
        if (!(d >= floor && d <= 100))
        {
            snprintf(err, n, "point %d: duty %g%% outside %g..100%% (FLOOR)", c.n + 1, d, floor);
            return -1;
        }
        if (c.n && t <= c.t[c.n - 1])
        {
            snprintf(err, n, "point %d: temperatures must rise", c.n + 1);
            return -1;
        }
        if (c.n && d < c.d[c.n - 1])
        {
            snprintf(err, n, "point %d: duty must not fall as the temperature rises", c.n + 1);
            return -1;
        }
        c.t[c.n] = t;
        c.d[c.n++] = d;
    }
    if (c.n < 2)
    {
        snprintf(err, n, "at least 2 points");
        return -1;
    }
    if (c.d[c.n - 1] != 100)
    {
        snprintf(err, n, "the last point must be 100%%");
        return -1;
    }
    *cv = c;
    return 0;
}

/* ------------------------------------------------------------------ config */
static const char *const curve_keys[NZONES][NGROUPS] = {
    { "CPU_PCS_CURVE", "CPU_VRM_CURVE", NULL, NULL },
    { "BOARD_PCS_CURVE", "BOARD_VRM_CURVE", "BOARD_IOHUB_CURVE", "BOARD_GPU_CURVE" },
};

const char *curve_key(int zone, int group)
{
    return zone >= 0 && zone < NZONES && group >= 0 && group < NGROUPS ? curve_keys[zone][group] : NULL;
}

/* The defaults. CPU: the vendor's curve of the board's device tree (thermal
 * zone cpu-thermal_0 on the PCS: active trips 40/50/55/60/65/70/75 °C with
 * 3 °C hysteresis, every fan at cooling levels 0/60/70/90/120/150/200/255 of
 * 255, i.e. 0/24/27/35/47/59/78/100 %), with its 0 % and 24 % below the
 * floor lifted to 30 % and at or above it everywhere else; full speed at
 * 75 °C, 10 °C below overheatd's limit. Board: the same CPU input a little
 * quieter below 70 °C; IOHUB (82-84 °C at full fans; warn 105, crit 110)
 * full speed at 98 °C; VRMs (ir38064, 62-74 °C) from 80 °C. */
static const char *const curve_defaults[NZONES][NGROUPS] = {
    { "45:30 55:38 60:47 65:60 70:80 75:100", "80:30 95:50 105:75 115:100", NULL, NULL },
    /* GPU: the IOHUB sits behind the RX 5700 XT (it went 82 -> 90 °C when
     * the GPU fans stopped, and 82 -> 56 °C with them at 40 %) */
    { "50:30 60:40 65:55 70:75 75:100", "80:30 95:50 105:75 115:100", "75:30 85:50 92:75 98:100", "65:30 75:45 85:70 95:100" },
};

void cfg_defaults(struct config *c)
{
    memset(c, 0, sizeof(*c));
    c->floor = 30;
    c->hyst = 3;
    c->slew_up = 10;
    c->slew_down = 1;
    c->poll_ms = 1000;
    c->stale_ms = 5000;
    c->settle_ms = 30000;
    c->start_full_ms = 15000;
    c->manual_timeout_ms = 600000;
    c->tach_min_rpm = 400;
    c->tach_alarm_ms = 8000;
    c->tach_verify_ms = 6000;
    c->tach_clear_ms = 60000;
    /* TACH_MODE accurate (LM96163 0x4A bits 1:0 = 10): on the board 885-945
     * rpm at 30 %, 1000-1050 at 40 %, 1557-1564 at 50 %, steady; in the
     * least-effect mode (11, what the platform sets) the readings swing and
     * drop to 0 below 50 % */
    c->tach_valid_duty = 30;
    c->tach_valid_duty_other = 50;
    c->tach_mode = 2;
    /* the X37 fan (measured): 1000-2400 rpm with a noisy tachometer at 30 %,
     * readings dropping to 0 at 40 %, a steady ~1650 at 50 %, saturated at
     * ~2700 from 60 % */
    c->cpu_fan_min = 35;
    c->cpu_fan_max = 65;
    c->board_fan_max = 100;
    c->case_fan_max = 100;
    c->cpu_tach = 1;
    c->emc_watchdog = 1;
    c->host_probe = 1;
    c->pcs_bus = 5;
    c->npcs = 7;
    for (int i = 0; i < 7; i++) c->pcs[i] = 0x20 + i;
    c->vrm_bus = 6;
    c->nvrm = 3;
    c->vrm[0] = 0x44;
    c->vrm[1] = 0x45;
    c->vrm[2] = 0x46;
    c->cpu_fan = (struct devaddr){ 6, 0x4c };
    c->board_fan = (struct devaddr){ 2, 0x4c };
    c->iohub = (struct devaddr){ 2, 0x4c };
    snprintf(c->gpu_file, sizeof(c->gpu_file), "/run/reimu-hostlink/gpu-temp");
    c->ncase = 2;
    c->case_fan[0] = (struct devaddr){ 2, 0x2c };
    c->case_fan[1] = (struct devaddr){ 2, 0x2e };
    char e[128];
    for (int z = 0; z < NZONES; z++)
        for (int g = 0; g < NGROUPS; g++)
            if (curve_defaults[z][g])
            {
                c->use[z][g] = 1;
                if (curve_parse(curve_defaults[z][g], &c->curve[z][g], c->floor, e, sizeof(e))) abort();
            }
}

static int parse_bool(const char *v, int *out)
{
    if (!strcasecmp(v, "yes") || !strcasecmp(v, "on") || !strcmp(v, "1") || !strcasecmp(v, "true")) *out = 1;
    else if (!strcasecmp(v, "no") || !strcasecmp(v, "off") || !strcmp(v, "0") || !strcasecmp(v, "false")) *out = 0;
    else return -1;
    return 0;
}

static int parse_num(const char *v, double lo, double hi, double *out)
{
    char *e;
    double d = strtod(v, &e);
    if (e == v || *e || !(d >= lo && d <= hi)) return -1;
    *out = d;
    return 0;
}

static int parse_dev(const char *v, struct devaddr *d)
{
    char *e;
    long bus = strtol(v, &e, 0);
    if (e == v || *e != ':' || bus < 0 || bus > 15) return -1;
    const char *p = e + 1;
    long a = strtol(p, &e, 0);
    if (e == p || (*e && !isspace((unsigned char)*e)) || a < 0x03 || a > 0x77) return -1;
    d->bus = bus;
    d->addr = a;
    return e - v;
}

/* "0x20 0x21 ..." or "none" (allow_none) */
static int parse_addrs(const char *v, int *out, int *n, int allow_none)
{
    int a[FAND_MAX_SENS], k = 0;
    if (!strcasecmp(v, "none"))
    {
        if (!allow_none) return -1;
        *n = 0;
        return 0;
    }
    const char *p = v;
    while (*p)
    {
        while (isspace((unsigned char)*p) || *p == ',') p++;
        if (!*p) break;
        char *e;
        long x = strtol(p, &e, 0);
        if (e == p || x < 0x03 || x > 0x77 || k == FAND_MAX_SENS) return -1;
        a[k++] = x;
        p = e;
    }
    if (!k) return -1;
    memcpy(out, a, sizeof(a[0]) * k);
    *n = k;
    return 0;
}

struct numkey
{
    const char *key;
    size_t off;
    int is_ms; /* config in seconds, kept in ms */
    int is_int;
    double lo, hi;
};

#define OFF(f) offsetof(struct config, f)
static const struct numkey numkeys[] = {
    { "FLOOR", OFF(floor), 0, 0, ABS_MIN_FLOOR, 100 },
    { "HYSTERESIS", OFF(hyst), 0, 0, 0, 10 },
    { "SLEW_UP", OFF(slew_up), 0, 0, 5, 100 },
    { "SLEW_DOWN", OFF(slew_down), 0, 0, 0.2, 100 },
    { "POLL_MS", OFF(poll_ms), 0, 1, 100, 5000 },
    { "STALE_SEC", OFF(stale_ms), 1, 1, 2, 60 },
    { "SETTLE_SEC", OFF(settle_ms), 1, 1, 0, 300 },
    { "START_FULL_SEC", OFF(start_full_ms), 1, 1, 0, 300 },
    { "MANUAL_TIMEOUT_SEC", OFF(manual_timeout_ms), 1, 1, 10, 86400 },
    { "TACH_MIN_RPM", OFF(tach_min_rpm), 0, 0, 50, 10000 },
    { "TACH_ALARM_SEC", OFF(tach_alarm_ms), 1, 1, 3, 60 },
    { "TACH_VERIFY_SEC", OFF(tach_verify_ms), 1, 1, 3, 60 },
    { "TACH_VALID_DUTY", OFF(tach_valid_duty), 0, 0, 0, 100 },
    { "TACH_VALID_DUTY_OTHER", OFF(tach_valid_duty_other), 0, 0, 0, 100 },
    { "CPU_FAN_MIN_DUTY", OFF(cpu_fan_min), 0, 0, ABS_MIN_FLOOR, 100 },
    { "CPU_FAN_MAX_DUTY", OFF(cpu_fan_max), 0, 0, 30, 100 },
    { "BOARD_FAN_MAX_DUTY", OFF(board_fan_max), 0, 0, 30, 100 },
    { "CASE_FAN_MAX_DUTY", OFF(case_fan_max), 0, 0, 30, 100 },
    { "TACH_CLEAR_SEC", OFF(tach_clear_ms), 1, 1, 5, 3600 },
    { "PCS_BUS", OFF(pcs_bus), 0, 1, 0, 15 },
    { "VRM_BUS", OFF(vrm_bus), 0, 1, 0, 15 },
};

static void add_err(char err[][128], int maxerr, int *nerr, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void add_err(char err[][128], int maxerr, int *nerr, const char *fmt, ...)
{
    if (*nerr < maxerr)
    {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err[*nerr], 128, fmt, ap);
        va_end(ap);
    }
    (*nerr)++;
}

static int is_curve_key(const char *k, int *z, int *g)
{
    for (*z = 0; *z < NZONES; (*z)++)
        for (*g = 0; *g < NGROUPS; (*g)++)
            if (curve_keys[*z][*g] && !strcmp(k, curve_keys[*z][*g])) return 1;
    return 0;
}

/* one KEY=VALUE; pass 0: everything but the curves, pass 1: the curves
 * (they are checked against the FLOOR of the same file) */
static void cfg_line(struct config *c, char *line, int lineno, int pass, char err[][128], int maxerr, int *nerr)
{
    char *eq = strchr(line, '=');
    if (!eq)
    {
        if (!pass) add_err(err, maxerr, nerr, "line %d: not KEY=VALUE", lineno);
        return;
    }
    *eq = 0;
    char *k = line, *v = eq + 1;
    while (*k && isspace((unsigned char)*k)) k++;
    for (char *e = k + strlen(k); e > k && isspace((unsigned char)e[-1]);) *--e = 0;
    while (*v && isspace((unsigned char)*v)) v++;
    for (char *e = v + strlen(v); e > v && isspace((unsigned char)e[-1]);) *--e = 0;
    size_t vl = strlen(v);
    if (vl >= 2 && (v[0] == '"' || v[0] == '\'') && v[vl - 1] == v[0])
    {
        v[vl - 1] = 0;
        v++;
    }
    int z, g;
    if (is_curve_key(k, &z, &g))
    {
        if (pass != 1) return;
        char e[100];
        if (!strcasecmp(v, "none") && g != G_PCS) c->use[z][g] = 0;
        else if (curve_parse(v, &c->curve[z][g], c->floor, e, sizeof(e)))
            add_err(err, maxerr, nerr, "%s: %s (kept %s)", k, e, c->use[z][g] ? "the previous curve" : "unused");
        else c->use[z][g] = 1;
        return;
    }
    if (pass) return;
    for (size_t i = 0; i < sizeof(numkeys) / sizeof(numkeys[0]); i++)
        if (!strcmp(k, numkeys[i].key))
        {
            double d;
            if (parse_num(v, numkeys[i].lo, numkeys[i].hi, &d) || (numkeys[i].is_int && d != floor(d)))
            {
                add_err(err, maxerr, nerr, "%s=%s: not a number in %g..%g", k, v, numkeys[i].lo, numkeys[i].hi);
                return;
            }
            char *p = (char *)c + numkeys[i].off;
            if (numkeys[i].is_int) *(int *)p = (int)(numkeys[i].is_ms ? d * 1000 : d);
            else *(double *)p = d;
            return;
        }
    int b;
    if (!strcmp(k, "CPU_FAN_TACH") || !strcmp(k, "EMC_WATCHDOG") || !strcmp(k, "HOST_PROBE"))
    {
        if (parse_bool(v, &b)) add_err(err, maxerr, nerr, "%s=%s: not yes/no", k, v);
        else if (k[0] == 'C') c->cpu_tach = b;
        else if (k[0] == 'E') c->emc_watchdog = b;
        else c->host_probe = b;
        return;
    }
    if (!strcmp(k, "GPU_TEMP_FILE"))
    {
        if (!*v || !strcasecmp(v, "none")) c->gpu_file[0] = 0;
        else if (v[0] != '/' || strlen(v) >= sizeof(c->gpu_file)) add_err(err, maxerr, nerr, "%s=%s: not an absolute path", k, v);
        else snprintf(c->gpu_file, sizeof(c->gpu_file), "%s", v);
        return;
    }
    if (!strcmp(k, "TACH_MODE"))
    {
        if (!strcasecmp(v, "accurate")) c->tach_mode = 2;
        else if (!strcasecmp(v, "least")) c->tach_mode = 3;
        else if (!strcasecmp(v, "traditional")) c->tach_mode = 0;
        else add_err(err, maxerr, nerr, "%s=%s: not accurate, least or traditional", k, v);
        return;
    }
    if (!strcmp(k, "PCS_ADDRS"))
    {
        if (parse_addrs(v, c->pcs, &c->npcs, 0)) add_err(err, maxerr, nerr, "%s=%s: not 1..8 addresses", k, v);
        return;
    }
    if (!strcmp(k, "VRM_ADDRS"))
    {
        if (parse_addrs(v, c->vrm, &c->nvrm, 1)) add_err(err, maxerr, nerr, "%s=%s: not 1..8 addresses or none", k, v);
        return;
    }
    struct devaddr d;
    if (!strcmp(k, "CPU_FAN") || !strcmp(k, "BOARD_FAN") || !strcmp(k, "IOHUB"))
    {
        int r = parse_dev(v, &d);
        if (!strcmp(k, "IOHUB") && !strcasecmp(v, "none")) c->iohub.bus = -1;
        else if (r < 0 || v[r]) add_err(err, maxerr, nerr, "%s=%s: not BUS:ADDR", k, v);
        else if (k[0] == 'C') c->cpu_fan = d;
        else if (k[0] == 'B') c->board_fan = d;
        else c->iohub = d;
        return;
    }
    if (!strcmp(k, "CASE_FANS"))
    {
        struct devaddr a[FAND_MAX_CASE];
        int n = 0;
        const char *p = v;
        if (strcasecmp(v, "none"))
            while (*p)
            {
                while (isspace((unsigned char)*p) || *p == ',') p++;
                if (!*p) break;
                int r = n < FAND_MAX_CASE ? parse_dev(p, &a[n]) : -1;
                if (r < 0)
                {
                    add_err(err, maxerr, nerr, "%s=%s: not up to %d BUS:ADDR or none", k, v, FAND_MAX_CASE);
                    return;
                }
                n++;
                p += r;
            }
        memcpy(c->case_fan, a, sizeof(a[0]) * n);
        c->ncase = n;
        return;
    }
    add_err(err, maxerr, nerr, "line %d: unknown key %s", lineno, k);
}

int cfg_parse(struct config *c, const char *text, char err[][128], int maxerr)
{
    int nerr = 0;
    for (int pass = 0; pass < 2; pass++)
    {
        const char *p = text;
        int lineno = 0;
        while (*p)
        {
            const char *e = strchr(p, '\n');
            size_t len = e ? (size_t)(e - p) : strlen(p);
            char line[512];
            lineno++;
            if (len < sizeof(line))
            {
                memcpy(line, p, len);
                line[len] = 0;
                char *s = line;
                while (isspace((unsigned char)*s)) s++;
                if (*s && *s != '#') cfg_line(c, s, lineno, pass, err, maxerr, &nerr);
            }
            else if (!pass) add_err(err, maxerr, &nerr, "line %d: too long", lineno);
            p += len + (e ? 1 : 0);
        }
    }
    /* a range can't end below its own start. A FLOOR at or above a fan's
     * maximum is fine: that fan runs at the FLOOR (FLOOR=100: always full) */
    if (c->cpu_fan_max < c->cpu_fan_min)
    {
        add_err(err, maxerr, &nerr, "CPU_FAN_MAX_DUTY %g below CPU_FAN_MIN_DUTY %g: 100", c->cpu_fan_max, c->cpu_fan_min);
        c->cpu_fan_max = 100;
    }
    /* the stale time must be longer than a poll */
    if (c->stale_ms < 2 * c->poll_ms)
    {
        add_err(err, maxerr, &nerr, "STALE_SEC shorter than 2 polls: set to %d ms", 2 * c->poll_ms);
        c->stale_ms = 2 * c->poll_ms;
    }
    return nerr;
}

/* no file: the defaults, silently */
int cfg_load(struct config *c, const char *file, char err[][128], int maxerr)
{
    int fd = open(file, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
    {
        if (errno == ENOENT) return 0;
        snprintf(err[0], 128, "%s: %s", file, strerror(errno));
        return 1;
    }
    char text[16384];
    ssize_t n = read(fd, text, sizeof(text) - 1);
    close(fd);
    if (n < 0 || n >= (ssize_t)sizeof(text) - 1)
    {
        snprintf(err[0], 128, "%s: unreadable or too big", file);
        return 1;
    }
    text[n] = 0;
    return cfg_parse(c, text, err, maxerr);
}

int cfg_save_key(const char *file, const char *key, const char *value)
{
    char old[16384], tmp[512];
    ssize_t n = 0;
    int fd = open(file, O_RDONLY | O_CLOEXEC);
    if (fd >= 0)
    {
        n = read(fd, old, sizeof(old) - 1);
        close(fd);
        if (n < 0 || n >= (ssize_t)sizeof(old) - 1) return -1;
    }
    old[n] = 0;
    snprintf(tmp, sizeof(tmp), "%s.new", file);
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    int done = 0;
    size_t kl = strlen(key);
    for (char *p = old; *p;)
    {
        char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        char *s = p;
        while (s < p + len && isspace((unsigned char)*s)) s++;
        char *q = s + kl;
        while (q < p + len && isspace((unsigned char)*q) && *q != '\n') q++;
        if ((size_t)(p + len - s) > kl && !strncmp(s, key, kl) && q < p + len && *q == '=')
        {
            if (!done) fprintf(f, "%s=\"%s\"\n", key, value);
            done = 1;
        }
        else fprintf(f, "%.*s\n", (int)len, p);
        p += len + (e ? 1 : 0);
    }
    if (!done) fprintf(f, "%s=\"%s\"\n", key, value);
    int r = fflush(f) || fsync(fileno(f)) ? -1 : 0;
    if (fclose(f)) r = -1;
    if (!r) r = rename(tmp, file);
    if (r) unlink(tmp);
    return r;
}

/* ------------------------------------------------------------------ names */
int zone_by_name(const char *s)
{
    for (int z = 0; z < NZONES; z++)
        if (!strcmp(s, zone_id[z])) return z;
    return -1;
}

int group_by_name(const char *s)
{
    for (int g = 0; g < NGROUPS; g++)
        if (!strcmp(s, group_id[g])) return g;
    return -1;
}

int mode_by_name(const char *s)
{
    for (int m = 0; m < 3; m++)
        if (!strcmp(s, mode_name[m])) return m;
    return -1;
}

/* ------------------------------------------------------------------ setup */
static void setup_outputs(struct fand *f)
{
    const struct config *c = &f->c;
    struct zone *z = &f->z[Z_CPU];
    z->nout = 1;
    z->out[0] = (struct output){ .kind = OUT_LM96163, .a = c->cpu_fan, .has_tach = c->cpu_tach, .rpm = NAN, .rpm_last = NAN,
                                 .min_duty = c->cpu_fan_min, .max_duty = c->cpu_fan_max, .duty = 100 };
    z = &f->z[Z_BOARD];
    z->nout = 1 + c->ncase;
    z->out[0] = (struct output){ .kind = OUT_LM96163, .a = c->board_fan, .rpm = NAN, .rpm_last = NAN, .min_duty = c->floor,
                                 .max_duty = c->board_fan_max, .duty = 100 };
    for (int i = 0; i < c->ncase; i++)
        z->out[1 + i] = (struct output){ .kind = OUT_EMC2305, .a = c->case_fan[i], .rpm = NAN, .rpm_last = NAN,
                                         .min_duty = c->floor, .max_duty = c->case_fan_max, .duty = 100 };
}

static void setup_groups(struct fand *f)
{
    const struct config *c = &f->c;
    memset(f->g, 0, sizeof(f->g));
    f->g[G_PCS].n = c->npcs;
    for (int i = 0; i < c->npcs; i++) f->g[G_PCS].s[i].addr = c->pcs[i];
    f->g[G_VRM].n = c->nvrm;
    for (int i = 0; i < c->nvrm; i++) f->g[G_VRM].s[i].addr = c->vrm[i];
    f->g[G_IOHUB].n = c->iohub.bus >= 0 ? 1 : 0;
    f->g[G_IOHUB].s[0].addr = c->iohub.addr;
    f->g[G_GPU].n = c->gpu_file[0] ? 1 : 0;
}

void fand_init(struct fand *f, const struct config *c, int64_t now)
{
    memset(f, 0, sizeof(*f));
    f->c = *c;
    f->now = now;
    f->host = HOST_PENDING;
    f->phase = PH_WAIT;
    f->on_since_hint = -1;
    f->led = -1;
    setup_groups(f);
    setup_outputs(f);
    for (int z = 0; z < NZONES; z++) f->z[z].duty = 100;
}

/* a new config (SIGHUP, Reload): what was read and driven at addresses that
 * stay is kept, so a reload neither blinds a zone nor raises an alarm */
void fand_reconfig(struct fand *f, const struct config *c)
{
    struct group og[NGROUPS];
    struct zone oz[NZONES];
    memcpy(og, f->g, sizeof(og));
    memcpy(oz, f->z, sizeof(oz));
    int bus_same[NGROUPS] = { c->pcs_bus == f->c.pcs_bus, c->vrm_bus == f->c.vrm_bus,
                              c->iohub.bus == f->c.iohub.bus && c->iohub.addr == f->c.iohub.addr };
    f->c = *c;
    setup_groups(f);
    setup_outputs(f);
    for (int g = 0; g < NGROUPS; g++)
        for (int i = 0; bus_same[g] && i < f->g[g].n; i++)
            for (int j = 0; j < og[g].n; j++)
                if (og[g].s[j].addr == f->g[g].s[i].addr) f->g[g].s[i] = og[g].s[j];
    for (int z = 0; z < NZONES; z++)
    {
        for (int g = 0; g < NGROUPS; g++) f->z[z].in[g].have = 0;
        for (int i = 0; i < f->z[z].nout; i++)
            for (int j = 0; j < oz[z].nout; j++)
            {
                struct output *n = &f->z[z].out[i], *o = &oz[z].out[j];
                if (n->kind == o->kind && n->a.bus == o->a.bus && n->a.addr == o->a.addr)
                {
                    int tach = n->has_tach;
                    double lo = n->min_duty, hi = n->max_duty;
                    *n = *o;
                    n->has_tach = tach;
                    n->min_duty = lo;
                    n->max_duty = hi;
                }
            }
    }
    f->gen++;
}

/* ------------------------------------------------------------------ sensors */
static double linear11(int w)
{
    int e = (w >> 11) & 0x1f, m = w & 0x7ff;
    if (e & 0x10) e -= 32;
    if (m & 0x400) m -= 2048;
    return ldexp(m, e);
}

static int lm96163_ident(int bus, int addr)
{
    int m = hw_rd_byte(bus, addr, 0xfe), d = m >= 0 ? hw_rd_byte(bus, addr, 0xff) : -1;
    if (d < 0) return 0;
    return m == 0x01 && d == 0x49 ? 1 : -1;
}

/* °C, NAN = no reading; *why: what went wrong */
static double read_sensor(struct fand *f, int g, struct sensor *s, const char **why)
{
    const struct config *c = &f->c;
    double v;
    *why = "no answer";
    if (g == G_PCS)
    {
        uint8_t b[2];
        if (hw_rd_plain2(c->pcs_bus, s->addr, b)) return NAN;
        unsigned raw = b[0] << 8 | b[1];
        if (!raw)
        {
            *why = "reads 0 (no sensor)";
            return NAN;
        }
        v = raw / 8.0;
    }
    else if (g == G_VRM)
    {
        int w = hw_rd_word(c->vrm_bus, s->addr, 0x8d); /* READ_TEMPERATURE_1 */
        if (w < 0) return NAN;
        v = linear11(w);
    }
    else
    {
        int bus = c->iohub.bus, a = c->iohub.addr;
        int id = lm96163_ident(bus, a);
        if (id <= 0)
        {
            if (id < 0) *why = "not an LM96163";
            return NAN;
        }
        int st = hw_rd_byte(bus, a, 0x02);
        if (st < 0) return NAN;
        if (st & 0x04)
        {
            *why = "remote diode open";
            return NAN;
        }
        /* MSB first: it latches the LSB. Unsigned (to 255 °C) unless 0 */
        int um = hw_rd_byte(bus, a, 0x31), ul = um >= 0 ? hw_rd_byte(bus, a, 0x32) : -1;
        if (ul < 0) return NAN;
        if (um || ul) v = um + (ul >> 5) * 0.125;
        else
        {
            int sm = hw_rd_byte(bus, a, 0x01), sl = sm >= 0 ? hw_rd_byte(bus, a, 0x10) : -1;
            if (sl < 0) return NAN;
            v = (int8_t)sm + (sl >> 5) * 0.125;
        }
    }
    if (!(v >= TEMP_MIN && v <= TEMP_MAX))
    {
        *why = "out of range";
        return NAN;
    }
    return v;
}

/* the host's GPU, from reimu-hostlink: an integer of m°C, the file not
 * older than 30 s; NAN = none (the host off, no agent, stale, garbage) */
static double read_gpu(const char *file)
{
    char s[32];
    struct stat st;
    int fd = open(file, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NAN;
    ssize_t n = fstat(fd, &st) ? -1 : read(fd, s, sizeof(s) - 1);
    close(fd);
    if (n <= 0) return NAN;
    time_t now = time(NULL);
    if (st.st_mtime > now + 5 || now - st.st_mtime > 30) return NAN;
    s[n] = 0;
    char *e;
    long v = strtol(s, &e, 10);
    if (e == s || (*e && *e != '\n')) return NAN;
    double t = v / 1000.0;
    return t >= TEMP_MIN && t <= TEMP_MAX ? t : NAN;
}

static void read_groups(struct fand *f)
{
    for (int g = 0; g < NGROUPS; g++)
    {
        struct group *gr = &f->g[g];
        int used = 0;
        for (int z = 0; z < NZONES; z++) used |= f->c.use[z][g];
        gr->valid = 1;
        gr->max = NAN;
        gr->why[0] = 0;
        if (!gr->n || !used) continue;
        if (g == G_GPU)
        {
            /* optional: never "failing", whatever the file says */
            gr->max = read_gpu(f->c.gpu_file);
            if (!isnan(gr->max))
            {
                gr->s[0].v = gr->max;
                gr->s[0].last_ok = f->now;
            }
            continue;
        }
        for (int i = 0; i < gr->n; i++)
        {
            struct sensor *s = &gr->s[i];
            const char *why;
            double v = read_sensor(f, g, s, &why);
            if (!isnan(v))
            {
                if (s->failing) fand_log(5, "%s sensor 0x%02x is read again: %.1f °C", group_id[g], s->addr, v);
                s->failing = 0;
                s->v = v;
                s->last_ok = f->now;
            }
            int fresh = s->last_ok && f->now - s->last_ok <= f->c.stale_ms;
            if (!fresh)
            {
                if (!s->failing && s->last_ok)
                    fand_log(3, "%s sensor 0x%02x: %s for %d s: its zones at 100%%", group_id[g], s->addr, why,
                             f->c.stale_ms / 1000);
                else if (!s->failing)
                    fand_log(3, "%s sensor 0x%02x: %s: its zones at 100%%", group_id[g], s->addr, why);
                s->failing = 1;
                if (gr->valid)
                    snprintf(gr->why, sizeof(gr->why), "%s 0x%02x: %s", group_name[g], s->addr, why);
                gr->valid = 0;
            }
            else if (isnan(gr->max) || s->v > gr->max) gr->max = s->v;
        }
    }
}

/* ------------------------------------------------------------------ outputs */
/* The LM96163 PWM value register (0x4C) is 6 bits wide, 100 % at
 * 2 * PWM_FREQ (0x4D, 5 bits): 46 with this board's 0x17. Only in the
 * high-resolution mode (ENHANCED 0x45 bit 4, the 360 kHz clock: 0x4A bit 3
 * clear, PWM_FREQ 8, as the lm63 driver tests it) is it 8 bits, 255 = 100 %.
 * The scale is read before every write. -1: unreadable */
static int lm96163_max(int b, int a, int cfg)
{
    int freq = hw_rd_byte(b, a, 0x4d), enh = freq >= 0 ? hw_rd_byte(b, a, 0x45) : -1;
    if (enh < 0) return -1;
    if ((enh & 0x10) && !(cfg & 0x08) && freq == 8) return 255;
    freq &= 0x1f;
    if (!freq) freq = 1;
    return 2 * freq;
}

/* duty -> chip. 0: written and read back */
static int write_lm96163(struct fand *f, struct output *o, double duty)
{
    int b = o->a.bus, a = o->a.addr;
    if (o->ident != 1)
    {
        int id = lm96163_ident(b, a);
        if (id < 0 && o->ident != -1) fand_log(3, "fan %d-%04x: not an LM96163, not driven", b, a);
        o->ident = id;
        if (id != 1) return -1;
        if (o->has_tach)
        {
            int cfg1 = hw_rd_byte(b, a, 0x03);
            if (cfg1 >= 0 && !(cfg1 & 0x04)) fand_log(3, "fan %d-%04x: its tachometer input is off (CONFIG 0x%02x)", b, a, cfg1);
        }
    }
    int cfg = hw_rd_byte(b, a, 0x4a);
    if (cfg < 0) return -1;
    /* bit 5 PWPGM off: the chip runs its lookup table; on: the PWM value
     * register. Bits 1:0, a fan with a tachometer: its TACH_MODE (the chip
     * forgets it when the platform powers off). Bits 3 (PWM clock), 4
     * (polarity) and PWM_FREQ are never touched */
    int want = cfg | 0x20;
    if (o->has_tach) want = (want & ~0x03) | (f->c.tach_mode & 0x03);
    if (want != cfg)
    {
        if (!(cfg & 0x20)) fand_log(4, "fan %d-%04x was in its automatic (lookup table) mode: set to manual", b, a);
        if (hw_wr_byte(b, a, 0x4a, want)) return -1;
        int rb = hw_rd_byte(b, a, 0x4a);
        if (rb < 0) return -1;
        cfg = rb;
    }
    if (o->has_tach)
    {
        static const char *const names[4] = { "traditional", "?", "accurate", "least" };
        int ok = (cfg & 0x03) == (f->c.tach_mode & 0x03);
        o->tach_mode_ok = ok && f->c.tach_mode == 2;
        if (ok && o->tach_mode_logged != 1)
            fand_log(o->tach_mode_logged ? 5 : 6, "fan %d-%04x: tachometer mode %s (0x4A 0x%02x)", b, a, names[cfg & 3], cfg);
        if (!ok && o->tach_mode_logged != 2)
            fand_log(4, "fan %d-%04x: tachometer mode %s not taken (0x4A reads 0x%02x): tachometer trusted from %g%% only",
                     b, a, names[f->c.tach_mode & 3], cfg, f->c.tach_valid_duty_other);
        o->tach_mode_logged = ok ? 1 : 2;
    }
    if (!(cfg & 0x20)) return -1;
    int max = lm96163_max(b, a, cfg);
    if (max < 0) return -1;
    int raw = duty >= 100 ? max : (int)lround(duty * max / 100);
    if (raw < 1) raw = 1; /* never 0 */
    if (raw > max) raw = max;
    if (hw_wr_byte(b, a, 0x4c, raw)) return -1;
    o->raw = raw;
    return hw_rd_byte(b, a, 0x4c) == raw ? 0 : -1;
}

static int write_emc2305(struct fand *f, struct output *o, double duty)
{
    int b = o->a.bus, a = o->a.addr;
    if (o->ident != 1)
    {
        int p = hw_rd_byte(b, a, 0xfd), m = p >= 0 ? hw_rd_byte(b, a, 0xfe) : -1;
        int id = m < 0 ? 0 : p == 0x34 && m == 0x5d ? 1 : -1;
        if (id < 0 && o->ident != -1) fand_log(3, "fan %d-%04x: not an EMC2305 (0x%02x 0x%02x), not driven", b, a, p, m);
        o->ident = id;
        if (id != 1) return -1;
        o->wd_on = 0;
        /* direct setting mode on every channel: with the RPM algorithm on,
         * the drive register isn't the drive */
        for (int n = 0; n < 5; n++)
        {
            int c1 = hw_rd_byte(b, a, 0x32 + 0x10 * n);
            if (c1 < 0) return -1;
            if (c1 & 0x80)
            {
                fand_log(4, "fan %d-%04x channel %d was in RPM mode: set to direct drive", b, a, n);
                if (hw_wr_byte(b, a, 0x32 + 0x10 * n, c1 & 0x7f)) return -1;
            }
        }
    }
    if (f->c.emc_watchdog && !o->wd_on)
    {
        /* CONFIGURATION WD_EN: watchdog in continuous mode, every fan to
         * full drive when no fan setting is written for 4 s */
        int cf = hw_rd_byte(b, a, 0x20);
        if (cf < 0 || (!(cf & 0x20) && hw_wr_byte(b, a, 0x20, cf | 0x20))) return -1;
        o->wd_on = 1;
    }
    int raw = duty >= 100 ? 0xff : (int)lround(duty * 255 / 100);
    if (raw < 1) raw = 1;
    int bad = 0;
    for (int n = 0; n < 5; n++)
    {
        int r = 0x30 + 0x10 * n;
        if (hw_wr_byte(b, a, r, raw) || hw_rd_byte(b, a, r) != raw)
        {
            bad = 1;
            break;
        }
    }
    o->raw = raw;
    return bad ? -1 : 0;
}

/* zone duty (FLOOR..100) -> this fan's duty: spread over its useful range,
 * 100 stays 100 (full scale). A range that is a point (its maximum at or
 * below the FLOOR or its minimum): that duty */
static double fan_duty(const struct output *o, double floor, double z)
{
    if (z >= 100) return 100;
    double lo = fmax(o->min_duty, floor), hi = o->max_duty;
    if (hi <= lo || floor >= 100) return lo;
    double d = lo + (z - floor) * (hi - lo) / (100 - floor);
    return fmin(fmax(d, lo), hi);
}

double fan_duty_test(const struct output *o, double floor, double z)
{
    return fan_duty(o, floor, z);
}

static void write_output(struct fand *f, struct output *o, double zduty)
{
    double duty = fan_duty(o, f->c.floor, zduty);
    o->duty = duty;
    if (fand_observe)
    {
        /* the chip's ID only */
        int id = o->kind == OUT_LM96163 ? lm96163_ident(o->a.bus, o->a.addr)
                 : hw_rd_byte(o->a.bus, o->a.addr, 0xfd) == 0x34 && hw_rd_byte(o->a.bus, o->a.addr, 0xfe) == 0x5d ? 1 : -1;
        o->ident = id;
        o->ok = id == 1;
        if (o->ok) o->last_ok = f->now;
        o->raw = o->kind == OUT_LM96163 ? hw_rd_byte(o->a.bus, o->a.addr, 0x4c) : hw_rd_byte(o->a.bus, o->a.addr, 0x30);
        return;
    }
    int r = o->kind == OUT_LM96163 ? write_lm96163(f, o, duty) : write_emc2305(f, o, duty);
    if (!r)
    {
        if (o->failing) fand_log(5, "fan %d-%04x is driven again", o->a.bus, o->a.addr);
        o->failing = 0;
        o->ok = 1;
        o->last_ok = f->now;
        return;
    }
    o->ok = 0;
    if (o->ident == 1) o->ident = 0; /* check it again once it answers */
}

/* LM96163 tachometer: count LSB 0x46 first (latches the MSB), MSB 0x47;
 * RPM = 5400000 / count, as the lm63 driver; 0 and 0xfffc = stopped */
static void read_tach(struct fand *f, struct output *o)
{
    int lo = hw_rd_byte(o->a.bus, o->a.addr, 0x46), hi = lo >= 0 ? hw_rd_byte(o->a.bus, o->a.addr, 0x47) : -1;
    if (hi < 0) return;
    int count = (hi << 8 | lo) & 0xfffc;
    double rpm = count == 0 || count == 0xfffc ? 0 : 5400000.0 / count;
    o->st[o->sp] = f->now;
    o->sv[o->sp] = rpm;
    o->sp = (o->sp + 1) % TACH_RING;
    if (o->sn < TACH_RING) o->sn++;
    o->rpm_last = rpm;
    o->tach_last_ok = f->now;
}

/* the best reading since `from`, -1 = none */
static double tach_max(const struct output *o, int64_t from)
{
    double m = -1;
    for (int i = 0; i < o->sn; i++)
    {
        int k = (o->sp - 1 - i + TACH_RING) % TACH_RING;
        if (o->st[k] < from) break;
        if (o->sv[k] > m) m = o->sv[k];
    }
    return m;
}

static void tach_alarm_on(struct fand *f, struct output *o, const char *what)
{
    int64_t now = f->now;
    f->tach_alarm = 1;
    f->tach_verify = 0;
    int recent = 1;
    for (int i = 0; i < 3; i++)
        if (f->tach_alarm_at[i] && now - f->tach_alarm_at[i] < 3600000) recent++;
    memmove(&f->tach_alarm_at[1], &f->tach_alarm_at[0], 2 * sizeof(f->tach_alarm_at[0]));
    f->tach_alarm_at[0] = now;
    f->tach_alarms++;
    if (recent >= 3) f->tach_latched = 1;
    fand_log(2, "CPU fan %d-%04x: %s: FAN FAILURE, all fans 100%%%s", o->a.bus, o->a.addr, what,
             f->tach_latched ? " (3rd time within an hour: latched until ClearAlarms)" : "");
    f->gen++;
}

/* A PWM-driven (3-wire) fan's tachometer is chopped by the PWM: at low duty
 * its readings are noisy and drop to 0 now and then. So a fan is judged by
 * its best reading within TACH_ALARM_SEC, and
 *   - at a duty of TACH_VALID_DUTY or more: best < TACH_MIN_RPM = failure
 *   - below: suspected; it runs at full speed for TACH_VERIFY_SEC (the
 *     first 2 s spinning up), where its tachometer is trusted: no reading
 *     of TACH_MIN_RPM there = failure, else back
 * A stopped fan is alarmed TACH_ALARM_SEC (+ TACH_VERIFY_SEC) after it
 * stopped: 8 (14) s by default. An unreadable tachometer reads as stopped. */
static void check_tach(struct fand *f, struct output *o)
{
    const struct config *c = &f->c;
    int64_t now = f->now;
    double w = tach_max(o, now - c->tach_alarm_ms);
    o->rpm = w < 0 ? NAN : w;
    char what[160];
    if (f->tach_alarm)
    {
        if (f->tach_latched) return;
        if (w >= c->tach_min_rpm)
        {
            if (!f->tach_ok_since) f->tach_ok_since = now;
            if (now - f->tach_ok_since >= c->tach_clear_ms)
            {
                f->tach_alarm = 0;
                o->tach_since = now;
                fand_log(5, "CPU fan %d-%04x: %.0f rpm for %d s: fan alarm cleared", o->a.bus, o->a.addr, w, c->tach_clear_ms / 1000);
                f->gen++;
            }
        }
        else f->tach_ok_since = 0;
        return;
    }
    if (f->tach_verify)
    {
        double v = tach_max(o, f->tach_verify_since + 2000);
        if (v >= c->tach_min_rpm)
        {
            f->tach_verify = 0;
            o->tach_since = now; /* judged on new readings only */
            fand_log(6, "CPU fan %d-%04x: %.0f rpm at full speed: fine", o->a.bus, o->a.addr, v);
            f->gen++;
        }
        else if (now - f->tach_verify_since >= c->tach_verify_ms)
        {
            snprintf(what, sizeof(what), "%s at full speed for %d s", v < 0 ? "tachometer unreadable" : "below the minimum",
                     c->tach_verify_ms / 1000);
            if (v >= 0) snprintf(what, sizeof(what), "%.0f rpm at most at full speed for %d s, below %.0f", v,
                                 c->tach_verify_ms / 1000, c->tach_min_rpm);
            tach_alarm_on(f, o, what);
        }
        return;
    }
    /* not yet a whole window of readings */
    if (now - o->tach_since < c->tach_alarm_ms) return;
    if (w >= c->tach_min_rpm) return;
    if (w < 0) snprintf(what, sizeof(what), "tachometer unreadable for %d s", c->tach_alarm_ms / 1000);
    else snprintf(what, sizeof(what), "%.0f rpm at most in %d s at %.0f%%, below %.0f", w, c->tach_alarm_ms / 1000, o->duty,
                  c->tach_min_rpm);
    double valid = o->tach_mode_ok ? fmax(c->tach_valid_duty, 0) : c->tach_valid_duty_other;
    if (o->duty >= valid)
    {
        tach_alarm_on(f, o, what);
        return;
    }
    f->tach_verify = 1;
    f->tach_verify_since = now;
    f->tach_checks++;
    fand_log(5, "CPU fan %d-%04x: %s (its tachometer is trusted from %g%%): checking it at full speed", o->a.bus, o->a.addr,
             what, valid);
    f->gen++;
}

/* ------------------------------------------------------------------ fail-safe */
/* the chip's ID, read up to 3 times: one failed transfer must not leave a
 * fan out of the fail-safe */
static int ident3(int kind, int b, int a)
{
    int id = 0;
    for (int k = 0; k < 3 && !id; k++)
    {
        if (kind == OUT_LM96163) id = lm96163_ident(b, a);
        else
        {
            int p = hw_rd_byte(b, a, 0xfd), m = p >= 0 ? hw_rd_byte(b, a, 0xfe) : -1;
            id = m < 0 ? 0 : p == 0x34 && m == 0x5d ? 1 : -1;
        }
    }
    return id;
}

void fand_failsafe(const struct config *c)
{
    struct devaddr lm[2] = { c->cpu_fan, c->board_fan };
    for (int i = 0; i < 2; i++)
    {
        int b = lm[i].bus, a = lm[i].addr;
        if (ident3(OUT_LM96163, b, a) != 1) continue;
        int cfg = -1, max = -1;
        for (int k = 0; k < 3 && cfg < 0; k++) cfg = hw_rd_byte(b, a, 0x4a);
        if (cfg >= 0 && !(cfg & 0x20)) hw_wr_byte(b, a, 0x4a, cfg | 0x20);
        for (int k = 0; k < 3 && max < 0 && cfg >= 0; k++) max = lm96163_max(b, a, cfg);
        /* scale unknown: 0x3f is full scale of the 6-bit register (100 % with
         * any PWM_FREQ); the 8-bit mode needs its read 0x4D */
        hw_wr_byte(b, a, 0x4c, max > 0 ? max : 0x3f);
    }
    for (int i = 0; i < c->ncase; i++)
    {
        int b = c->case_fan[i].bus, a = c->case_fan[i].addr;
        if (ident3(OUT_EMC2305, b, a) != 1) continue;
        for (int n = 0; n < 5; n++) hw_wr_byte(b, a, 0x30 + 0x10 * n, 0xff);
    }
}

/* the fan chips' ID registers, the first PCS sensor: any of them answering
 * means the platform is powered */
int fand_probe(const struct config *c)
{
    uint8_t b[2];
    if (hw_rd_byte(c->cpu_fan.bus, c->cpu_fan.addr, 0xfe) >= 0) return 1;
    if (hw_rd_byte(c->board_fan.bus, c->board_fan.addr, 0xfe) >= 0) return 1;
    return c->npcs && !hw_rd_plain2(c->pcs_bus, c->pcs[0], b);
}

/* ------------------------------------------------------------------ control */
static void set_led(struct fand *f, int on)
{
    if (f->led == on || fand_observe) return;
    if (hw_led(LED_NAME, on))
    {
        if (f->led != -2) fand_log(4, "can't set LED %s", LED_NAME);
        f->led = -2; /* retried next poll */
        return;
    }
    f->led = on;
}

static void zone_step(struct fand *f, int zi, double dt)
{
    struct zone *z = &f->z[zi];
    const struct config *c = &f->c;
    int failsafe = 0, hot = 0;
    char reason[128] = "";
    double demand = c->floor;
    for (int g = 0; g < NGROUPS; g++)
    {
        struct zin *in = &z->in[g];
        if (!c->use[zi][g] || !f->g[g].n) continue;
        if (!f->g[g].valid)
        {
            if (!failsafe) snprintf(reason, sizeof(reason), "нет данных: %s", f->g[g].why);
            failsafe = 1;
            in->have = 0;
            in->demand = 100;
            continue;
        }
        double t = f->g[g].max;
        if (g == G_GPU && isnan(t))
        {
            /* an optional input without a reading: no demand */
            in->have = 0;
            in->demand = NAN;
            continue;
        }
        if (!in->have || t >= in->t_eff) in->t_eff = t;
        else if (t <= in->t_eff - c->hyst) in->t_eff = t + c->hyst;
        in->have = 1;
        const struct curve *cv = &c->curve[zi][g];
        in->demand = curve_eval(cv, in->t_eff);
        if (in->demand > demand) demand = in->demand;
        if (in->t_eff >= cv->t[cv->n - 1]) hot = 1;
    }
    if (demand > 100) demand = 100;
    z->demand = demand;

    double target;
    int now_ = 1; /* no slew */
    if (failsafe) target = 100;
    else if (f->tach_alarm || f->out_alarm)
    {
        target = 100;
        snprintf(reason, sizeof(reason), "%s", f->tach_alarm ? "отказ вентилятора процессора" : "вентилятор не управляется");
    }
    else if (f->turbo_until > f->now)
    {
        target = 100;
        snprintf(reason, sizeof(reason), "турбо");
    }
    else if (zi == Z_CPU && f->tach_verify)
    {
        target = 100;
        snprintf(reason, sizeof(reason), "проверка тахометра на полной скорости");
    }
    else if (z->mode == MODE_FULL)
    {
        target = 100;
        snprintf(reason, sizeof(reason), "режим 100 %%");
    }
    else if (f->now < f->full_until)
    {
        target = 100;
        snprintf(reason, sizeof(reason), "запуск: 100 %%");
    }
    else if (z->mode == MODE_MANUAL)
    {
        target = hot ? 100 : z->manual_duty;
        if (hot) snprintf(reason, sizeof(reason), "ручной режим, но вход достиг 100 %% кривой");
    }
    else
    {
        target = demand;
        now_ = 0;
    }
    if (target < c->floor) target = c->floor;
    if (target > 100) target = 100;
    if (now_) z->duty = target;
    else if (target > z->duty) z->duty = fmin(target, z->duty + c->slew_up * dt);
    else z->duty = fmax(target, z->duty - c->slew_down * dt);
    if (z->duty < c->floor) z->duty = c->floor;

    if (failsafe != z->failsafe || hot != z->hot) f->gen++;
    if (failsafe && !z->failsafe) fand_log(3, "zone %s: %s: 100%%", zone_id[zi], reason);
    if (!failsafe && z->failsafe) fand_log(5, "zone %s: inputs back, automatic control", zone_id[zi]);
    z->failsafe = failsafe;
    z->hot = hot;
    snprintf(z->reason, sizeof(z->reason), "%s", reason);
}

static void manual_timeouts(struct fand *f)
{
    for (int zi = 0; zi < NZONES; zi++)
    {
        struct zone *z = &f->z[zi];
        if (z->mode == MODE_MANUAL && f->now >= z->manual_until)
        {
            z->mode = MODE_AUTO;
            fand_log(5, "zone %s: manual mode not confirmed for %d s: back to auto", zone_id[zi], f->c.manual_timeout_ms / 1000);
            f->gen++;
        }
    }
}

static void enter_off(struct fand *f)
{
    if (f->phase == PH_RUN)
    {
        /* one try: if the host is not really off, the fans are at 100 % */
        if (!fand_observe) fand_failsafe(&f->c);
        fand_log(5, "host off: fans unpowered, idle");
    }
    f->phase = PH_OFF;
    f->out_alarm = 0;
    f->tach_verify = 0;
    if (!f->tach_latched) f->tach_alarm = 0;
    f->tach_low_since = f->tach_ok_since = 0;
    for (int zi = 0; zi < NZONES; zi++)
    {
        f->z[zi].failsafe = 0;
        f->z[zi].reason[0] = 0;
    }
    f->gen++;
}

static void enter_run(struct fand *f)
{
    f->phase = PH_RUN;
    f->run_since = f->now;
    f->tach_verify = 0;
    f->full_until = f->now + f->c.start_full_ms;
    f->tach_low_since = f->tach_ok_since = 0;
    for (int g = 0; g < NGROUPS; g++)
        for (int i = 0; i < f->g[g].n; i++)
        {
            f->g[g].s[i].last_ok = 0;
            f->g[g].s[i].failing = 0;
        }
    for (int zi = 0; zi < NZONES; zi++)
    {
        struct zone *z = &f->z[zi];
        z->duty = 100;
        for (int g = 0; g < NGROUPS; g++) z->in[g].have = 0;
        for (int i = 0; i < z->nout; i++)
        {
            struct output *o = &z->out[i];
            o->ident = 0;
            o->ok = 0;
            o->last_ok = 0;
            o->failing = 0;
            o->wd_on = 0;
            o->sn = o->sp = 0;
            o->tach_mode_ok = 0;
            o->rpm = o->rpm_last = NAN;
            o->tach_since = f->now;
            o->tach_last_ok = 0;
        }
    }
    fand_log(5, "host on for %lld s: fan control takes over, 100%% for %d s", (long long)((f->now - f->on_since) / 1000),
             f->c.start_full_ms / 1000);
    f->gen++;
}

void fand_step(struct fand *f, int64_t now, int host)
{
    double dt = f->last_step ? (now - f->last_step) / 1000.0 : 0;
    if (dt < 0) dt = 0;
    if (dt > 5) dt = 5;
    f->last_step = now;
    f->now = now;
    f->ticks++;
    if (host != f->host) f->gen++;
    f->host = host;
    manual_timeouts(f);
    if (f->turbo_until && now >= f->turbo_until)
    {
        f->turbo_until = 0;
        fand_log(5, "turbo over: back to the curves");
        f->gen++;
    }

    if (host == HOST_PENDING)
    {
        if (f->phase != PH_WAIT && f->phase != PH_OFF) enter_off(f);
        f->phase = PH_WAIT;
        return;
    }
    if (host == HOST_OFF)
    {
        if (f->phase != PH_OFF) enter_off(f);
        set_led(f, f->tach_latched);
        return;
    }
    if (f->phase == PH_OFF || f->phase == PH_WAIT)
    {
        f->phase = PH_SETTLE;
        f->on_since = now;
        if (f->on_since_hint >= 0 && f->on_since_hint <= now) f->on_since = f->on_since_hint;
        f->on_since_hint = -1;
        if (now - f->on_since < f->c.settle_ms)
            fand_log(5, "host on: the bus is left alone for %lld s", (long long)((f->c.settle_ms - (now - f->on_since)) / 1000));
        f->gen++;
    }
    if (f->phase == PH_SETTLE)
    {
        if (now - f->on_since < f->c.settle_ms) return;
        enter_run(f);
    }

    /* PH_RUN */
    read_groups(f);
    struct output *cpu = &f->z[Z_CPU].out[0];
    /* the tachometer whatever became of the last write: only not on a chip
     * known to be another one */
    if (cpu->has_tach && cpu->ident != -1) read_tach(f, cpu);
    if (cpu->has_tach) check_tach(f, cpu);
    for (int zi = 0; zi < NZONES; zi++) zone_step(f, zi, dt);

    int out_alarm = 0;
    for (int zi = 0; zi < NZONES; zi++)
    {
        struct zone *z = &f->z[zi];
        for (int i = 0; i < z->nout; i++)
        {
            struct output *o = &z->out[i];
            write_output(f, o, z->duty);
            int64_t since = o->last_ok ? o->last_ok : f->run_since;
            if (o->ident == -1 || (!o->ok && now - since > f->c.stale_ms))
            {
                if (!o->failing)
                    fand_log(2, "fan %d-%04x not driven for %d s: FAN CONTROL FAILURE, the other fans 100%%", o->a.bus, o->a.addr,
                             (int)((now - since) / 1000));
                o->failing = 1;
                out_alarm = 1;
            }
        }
    }
    if (out_alarm != f->out_alarm) f->gen++;
    f->out_alarm = out_alarm;

    int fault = f->tach_alarm || f->out_alarm;
    for (int zi = 0; zi < NZONES; zi++) fault |= f->z[zi].failsafe;
    set_led(f, fault);
}

/* ------------------------------------------------------------------ requests */
static int check_zone(int zone, char *err, size_t n)
{
    if (zone < 0 || zone >= NZONES)
    {
        snprintf(err, n, "no such zone (cpu, board)");
        return -1;
    }
    return 0;
}

int fand_set_mode(struct fand *f, int zone, int mode, char *err, size_t n)
{
    if (check_zone(zone, err, n)) return -1;
    if (mode < 0 || mode > MODE_FULL)
    {
        snprintf(err, n, "no such mode (auto, manual, full)");
        return -1;
    }
    struct zone *z = &f->z[zone];
    if (mode == MODE_MANUAL)
    {
        if (z->mode != MODE_MANUAL) z->manual_duty = fmax(z->duty, f->c.floor);
        z->manual_until = f->now + f->c.manual_timeout_ms;
    }
    if (mode != z->mode) fand_log(5, "zone %s: mode %s", zone_id[zone], mode_name[mode]);
    z->mode = mode;
    f->gen++;
    return 0;
}

int fand_set_duty(struct fand *f, int zone, double duty, char *err, size_t n)
{
    if (check_zone(zone, err, n)) return -1;
    if (!(duty >= f->c.floor && duty <= 100))
    {
        snprintf(err, n, "duty %g%% outside %g..100%%", duty, f->c.floor);
        return -1;
    }
    struct zone *z = &f->z[zone];
    z->manual_duty = duty;
    z->manual_until = f->now + f->c.manual_timeout_ms;
    if (z->mode != MODE_MANUAL) fand_log(5, "zone %s: mode manual", zone_id[zone]);
    fand_log(6, "zone %s: manual duty %g%%", zone_id[zone], duty);
    z->mode = MODE_MANUAL;
    f->gen++;
    return 0;
}

int fand_confirm(struct fand *f, int zone, char *err, size_t n)
{
    if (check_zone(zone, err, n)) return -1;
    struct zone *z = &f->z[zone];
    if (z->mode != MODE_MANUAL)
    {
        snprintf(err, n, "zone %s is not in manual mode", zone_id[zone]);
        return -1;
    }
    z->manual_until = f->now + f->c.manual_timeout_ms;
    return 0;
}

int fand_set_curve(struct fand *f, int zone, int group, const char *pts, char *err, size_t n)
{
    if (check_zone(zone, err, n)) return -1;
    if (!curve_key(zone, group))
    {
        snprintf(err, n, "zone %s has no input %s", zone_id[zone], group >= 0 && group < NGROUPS ? group_id[group] : "?");
        return -1;
    }
    struct curve cv;
    if (curve_parse(pts, &cv, f->c.floor, err, n)) return -1;
    f->c.curve[zone][group] = cv;
    f->c.use[zone][group] = 1;
    f->z[zone].in[group].have = 0;
    f->gen++;
    return 0;
}

void fand_clear_alarms(struct fand *f)
{
    f->tach_alarm = f->tach_latched = f->tach_verify = 0;
    memset(f->tach_alarm_at, 0, sizeof(f->tach_alarm_at));
    for (int zi = 0; zi < NZONES; zi++)
        for (int i = 0; i < f->z[zi].nout; i++) f->z[zi].out[i].tach_since = f->now;
    f->tach_low_since = f->tach_ok_since = 0;
    f->out_alarm = 0;
    for (int zi = 0; zi < NZONES; zi++)
        for (int i = 0; i < f->z[zi].nout; i++)
        {
            f->z[zi].out[i].failing = 0;
            if (f->z[zi].out[i].ident == -1) f->z[zi].out[i].ident = 0;
        }
    fand_log(5, "alarms cleared");
    f->gen++;
}

int fand_set_turbo(struct fand *f, unsigned minutes, char *err, size_t n)
{
    if (minutes == FAND_TURBO_UNTIL_OFF) minutes = FAND_TURBO_MAX_MIN;
    else if (minutes > FAND_TURBO_MAX_MIN)
    {
        snprintf(err, n, "turbo %u min: 1..%d, 0 = off, %u = until off (%d h at most)", minutes, FAND_TURBO_MAX_MIN,
                 FAND_TURBO_UNTIL_OFF, FAND_TURBO_MAX_MIN / 60);
        return -1;
    }
    if (!minutes)
    {
        if (f->turbo_until) fand_log(5, "turbo off: back to the curves");
        f->turbo_until = 0;
    }
    else
    {
        f->turbo_until = f->now + (int64_t)minutes * 60000;
        fand_log(5, "turbo: every fan full scale for %u min", minutes);
    }
    f->gen++;
    return 0;
}

int fand_turbo_left(const struct fand *f)
{
    return f->turbo_until > f->now ? (int)((f->turbo_until - f->now + 999) / 1000) : 0;
}

int fand_alarm(const struct fand *f)
{
    if (f->tach_alarm || f->out_alarm) return 1;
    for (int zi = 0; zi < NZONES; zi++)
        if (f->z[zi].failsafe) return 1;
    return 0;
}

void fand_alarm_text(const struct fand *f, char *buf, size_t n)
{
    size_t len = 0;
    buf[0] = 0;
#define ADD(...) \
    do { if (len < n) len += snprintf(buf + len, n - len, "%s", len ? "; " : ""); \
         if (len < n) len += snprintf(buf + len, n - len, __VA_ARGS__); } while (0)
    if (f->tach_alarm)
        ADD("отказ вентилятора процессора: %.0f об/мин%s", isnan(f->z[Z_CPU].out[0].rpm) ? 0 : f->z[Z_CPU].out[0].rpm,
            f->tach_latched ? " (зафиксирован)" : "");
    for (int zi = 0; zi < NZONES; zi++)
        for (int i = 0; i < f->z[zi].nout; i++)
            if (f->z[zi].out[i].failing) ADD("нет управления вентилятором %d-%04x", f->z[zi].out[i].a.bus, f->z[zi].out[i].a.addr);
    for (int zi = 0; zi < NZONES; zi++)
        if (f->z[zi].failsafe) ADD("зона %s: %s", zone_id[zi], f->z[zi].reason);
#undef ADD
}

const char *fand_host_name(const struct fand *f)
{
    switch (f->phase)
    {
    case PH_OFF: return "off";
    case PH_SETTLE: return "starting";
    case PH_RUN: return f->host == HOST_UNKNOWN ? "unknown" : "on";
    default: return "pending";
    }
}

/* ------------------------------------------------------------------ JSON */
static void json_curve(struct buf *b, const struct curve *cv)
{
    buf_printf(b, "[");
    for (int i = 0; i < cv->n; i++) buf_printf(b, "%s[%g,%g]", i ? "," : "", cv->t[i], cv->d[i]);
    buf_printf(b, "]");
}

void fand_json(const struct fand *f, struct buf *b)
{
    const struct config *c = &f->c;
    char at[512];
    fand_alarm_text(f, at, sizeof(at));
    buf_printf(b, "{\"version\":1,\"host\":\"%s\",", fand_host_name(f));
    if (f->phase == PH_SETTLE)
        buf_printf(b, "\"settle_left\":%lld,", (long long)((c->settle_ms - (f->now - f->on_since) + 999) / 1000));
    if (f->phase == PH_RUN && f->now < f->full_until)
        buf_printf(b, "\"full_left\":%lld,", (long long)((f->full_until - f->now + 999) / 1000));
    buf_printf(b, "\"turbo\":%s,\"turbo_left\":%d,\"turbo_max_min\":%d,", fand_turbo_left(f) ? "true" : "false",
               fand_turbo_left(f), FAND_TURBO_MAX_MIN);
    buf_printf(b, "\"floor\":%g,\"hysteresis\":%g,\"slew_up\":%g,\"slew_down\":%g,\"manual_timeout\":%d,", c->floor, c->hyst,
               c->slew_up, c->slew_down, c->manual_timeout_ms / 1000);
    buf_printf(b, "\"alarm\":%s,\"alarm_text\":", fand_alarm(f) ? "true" : "false");
    buf_jstr(b, at);
    buf_printf(b, ",\"tach_alarm\":%s,\"tach_latched\":%s,\"tach_verify\":%s,\"tach_checks\":%u,\"config_errors\":[",
               f->tach_alarm ? "true" : "false", f->tach_latched ? "true" : "false", f->tach_verify ? "true" : "false",
               f->tach_checks);
    for (int i = 0; i < f->nerr && i < FAND_MAX_ERR; i++)
    {
        if (i) buf_printf(b, ",");
        buf_jstr(b, f->err[i]);
    }
    buf_printf(b, "],\"inputs\":[");
    for (int g = 0, first = 1; g < NGROUPS; g++)
    {
        const struct group *gr = &f->g[g];
        if (!gr->n) continue;
        buf_printf(b, "%s{\"id\":\"%s\",\"name\":", first ? "" : ",", group_id[g]);
        first = 0;
        buf_jstr(b, group_name[g]);
        int run = f->phase == PH_RUN;
        buf_printf(b, ",\"valid\":%s,\"temp\":", run && gr->valid ? "true" : "false");
        buf_num(b, run && gr->valid ? gr->max : NAN, 1);
        buf_printf(b, ",\"why\":");
        buf_jstr(b, run ? gr->why : "");
        if (g == G_GPU)
        {
            buf_printf(b, ",\"optional\":true,\"file\":");
            buf_jstr(b, c->gpu_file);
        }
        buf_printf(b, ",\"sensors\":[");
        for (int i = 0; g != G_GPU && i < gr->n; i++)
        {
            const struct sensor *s = &gr->s[i];
            int fresh = run && s->last_ok && f->now - s->last_ok <= c->stale_ms;
            buf_printf(b, "%s{\"addr\":\"0x%02x\",\"temp\":", i ? "," : "", s->addr);
            buf_num(b, fresh ? s->v : NAN, 2);
            buf_printf(b, "}");
        }
        buf_printf(b, "]}");
    }
    buf_printf(b, "],\"zones\":[");
    for (int zi = 0; zi < NZONES; zi++)
    {
        const struct zone *z = &f->z[zi];
        int run = f->phase == PH_RUN;
        buf_printf(b, "%s{\"id\":\"%s\",\"name\":", zi ? "," : "", zone_id[zi]);
        buf_jstr(b, zone_name[zi]);
        buf_printf(b, ",\"mode\":\"%s\",\"duty\":", mode_name[z->mode]);
        buf_num(b, run ? z->duty : NAN, 1);
        buf_printf(b, ",\"demand\":");
        buf_num(b, run ? z->demand : NAN, 1);
        buf_printf(b, ",\"manual_duty\":");
        buf_num(b, z->mode == MODE_MANUAL ? z->manual_duty : NAN, 1);
        if (z->mode == MODE_MANUAL) buf_printf(b, ",\"manual_left\":%lld", (long long)((z->manual_until - f->now + 999) / 1000));
        buf_printf(b, ",\"failsafe\":%s,\"hot\":%s,\"reason\":", z->failsafe ? "true" : "false", z->hot ? "true" : "false");
        buf_jstr(b, run ? z->reason : "");
        buf_printf(b, ",\"inputs\":[");
        for (int g = 0, first = 1; g < NGROUPS; g++)
        {
            if (!curve_keys[zi][g]) continue;
            char key[24];
            snprintf(key, sizeof(key), "%s", curve_keys[zi][g]);
            buf_printf(b, "%s{\"id\":\"%s\",\"key\":\"%s\",\"used\":%s,\"temp\":", first ? "" : ",", group_id[g], key,
                       c->use[zi][g] && f->g[g].n ? "true" : "false");
            first = 0;
            buf_num(b, run && z->in[g].have ? z->in[g].t_eff : NAN, 1);
            buf_printf(b, ",\"demand\":");
            buf_num(b, run && z->in[g].have ? z->in[g].demand : NAN, 1);
            buf_printf(b, ",\"curve\":");
            json_curve(b, &c->curve[zi][g]);
            buf_printf(b, "}");
        }
        buf_printf(b, "],\"fans\":[");
        for (int i = 0; i < z->nout; i++)
        {
            const struct output *o = &z->out[i];
            buf_printf(b, "%s{\"dev\":\"%d-%04x\",\"chip\":\"%s\",\"ok\":%s,\"raw\":%d", i ? "," : "", o->a.bus, o->a.addr,
                       o->kind == OUT_LM96163 ? "lm96163" : "emc2305", run && o->ok ? "true" : "false", o->raw);
            buf_printf(b, ",\"duty\":");
            buf_num(b, run ? o->duty : NAN, 1);
            buf_printf(b, ",\"range\":[%g,%g]", fmax(o->min_duty, c->floor), o->max_duty);
            if (o->has_tach)
            {
                buf_printf(b, ",\"rpm\":");
                buf_num(b, run ? o->rpm : NAN, 0);
                buf_printf(b, ",\"rpm_last\":");
                buf_num(b, run ? o->rpm_last : NAN, 0);
                buf_printf(b, ",\"tach_mode\":\"%s\",\"tach_mode_ok\":%s",
                           c->tach_mode == 2 ? "accurate" : c->tach_mode == 3 ? "least" : "traditional",
                           run && o->tach_mode_ok ? "true" : "false");
            }
            buf_printf(b, "}");
        }
        buf_printf(b, "]}");
    }
    buf_printf(b, "]}");
}
