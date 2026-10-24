/* reimu-fand: the control logic (fand.c) and the hardware access (fand-hw.c),
 * shared by the daemon (reimu-fand.c) and the native tests (tests/).
 *
 * Copyright (c) 2026 MCST REIMU contributors. Licensed under GPLv2.
 */
#ifndef FAND_H
#define FAND_H

#include <stddef.h>
#include <stdint.h>

#define FAND_MAX_POINTS 8
#define FAND_MAX_SENS   8
#define FAND_MAX_CASE   2
#define FAND_MAX_OUT    (1 + FAND_MAX_CASE)
#define FAND_MAX_ERR    12
/* Турбо: every fan at full scale for 1..FAND_TURBO_MAX_MIN minutes, or
 * FAND_TURBO_UNTIL_OFF (= until turned off, at most FAND_TURBO_MAX_MIN) */
#define FAND_TURBO_MAX_MIN   720
#define FAND_TURBO_UNTIL_OFF 0xffffffffu

/* temperature inputs: groups of sensors read once per poll, shared by the zones */
/* G_GPU: the host's GPU (reimu-hostlink), optional: never a fail-safe */
enum { G_PCS, G_VRM, G_IOHUB, G_GPU, NGROUPS };
/* cooling zones */
enum { Z_CPU, Z_BOARD, NZONES };
enum { MODE_AUTO, MODE_MANUAL, MODE_FULL };
/* host power as the daemon sees it: PENDING = not known yet (no bus traffic),
 * UNKNOWN = can't be known (taken as on) */
enum { HOST_PENDING, HOST_OFF, HOST_ON, HOST_UNKNOWN };
enum { PH_WAIT, PH_OFF, PH_SETTLE, PH_RUN };
enum { OUT_LM96163, OUT_EMC2305 };

/* PWM duty (%) against temperature (°C), linear between the points */
struct curve
{
    int n;
    double t[FAND_MAX_POINTS], d[FAND_MAX_POINTS];
};

struct devaddr
{
    int bus, addr;
};

struct config
{
    double floor;     /* % the fans never go below */
    double hyst;      /* °C a temperature must fall before the duty does */
    double slew_up;   /* %/s */
    double slew_down; /* %/s */
    int poll_ms, stale_ms, settle_ms, start_full_ms, manual_timeout_ms;
    double tach_min_rpm;
    int tach_alarm_ms;     /* the window: a fan is judged by its best reading in it */
    int tach_verify_ms;    /* suspected below TACH_VALID_DUTY: this long at full speed */
    int tach_clear_ms;
    double tach_valid_duty;       /* fan duty from which its tachometer is trusted ... */
    double tach_valid_duty_other; /* ... and when the chip isn't in the accurate mode */
    int tach_mode;                /* LM96163 0x4A bits 1:0 for a fan with a tachometer */
    /* each fan's useful duty range: the zone's FLOOR..100 % is spread over
     * it, 100 % is always full scale (a fan that saturates at 60 % would
     * otherwise run at full speed over half of every curve) */
    double cpu_fan_min, cpu_fan_max, board_fan_max, case_fan_max;
    int cpu_tach;     /* the CPU fan has a tachometer: check it */
    int emc_watchdog; /* EMC2305 watchdog: fans 100% when not written for 4 s */
    int host_probe;   /* host state Off/unknown: probe the platform devices */
    int pcs_bus, npcs, pcs[FAND_MAX_SENS];
    int vrm_bus, nvrm, vrm[FAND_MAX_SENS];
    struct devaddr cpu_fan, board_fan, iohub;
    char gpu_file[128]; /* the GPU temperature (m°C) from the host, "" = none */
    int ncase;
    struct devaddr case_fan[FAND_MAX_CASE];
    struct curve curve[NZONES][NGROUPS];
    int use[NZONES][NGROUPS];
};

struct sensor
{
    int addr;
    double v;        /* last good reading, °C */
    int64_t last_ok; /* ms */
    int failing;     /* reported */
};

struct group
{
    int n;
    struct sensor s[FAND_MAX_SENS];
    int valid;
    double max;
    char why[96];
};

struct zin
{
    int have;
    double t_eff; /* the temperature after hysteresis */
    double demand;
};

struct output
{
    int kind;
    struct devaddr a;
    int ident;       /* 1 = the chip is what it should be, -1 = it is not, 0 = not checked */
    int ok;          /* the last write was read back */
    int64_t last_ok;
    int failing;     /* reported */
    int raw;         /* register value written */
    double min_duty, max_duty; /* its useful range (config) */
    double duty;     /* fan duty written, % */
    int wd_on;       /* EMC2305 watchdog enabled */
    int has_tach;
    int tach_mode_ok;     /* the chip has TACH_MODE "accurate" (read back) */
    int tach_mode_logged; /* 1 set, 2 refused: reported */
    double rpm;      /* best reading in the window, NAN = none */
    double rpm_last;
#define TACH_RING 256
    int64_t st[TACH_RING]; /* tachometer samples: time, rpm */
    float sv[TACH_RING];
    int sn, sp;
    int64_t tach_since;    /* samples taken since then */
    int64_t tach_last_ok;
};

struct zone
{
    int mode;
    double manual_duty;
    int64_t manual_until;
    double duty;   /* applied */
    double demand; /* of the curves */
    int failsafe;  /* an input is missing: 100 % */
    int hot;       /* an input reached its curve's full-speed point */
    char reason[128];
    struct zin in[NGROUPS];
    int nout;
    struct output out[FAND_MAX_OUT];
};

struct fand
{
    struct config c;
    char err[FAND_MAX_ERR][128]; /* config errors */
    int nerr;
    struct group g[NGROUPS];
    struct zone z[NZONES];
    int host, phase;
    int64_t now, last_step, on_since, run_since, full_until, pending_since;
    int64_t on_since_hint; /* from the run file: the host was on since then (ms), -1 none */
    int tach_alarm, tach_latched, tach_alarms;
    int tach_verify; /* the CPU fan at full speed to check its tachometer */
    int64_t tach_verify_since;
    unsigned tach_checks;
    int64_t tach_low_since, tach_ok_since, tach_alarm_at[3];
    int out_alarm;
    int64_t turbo_until; /* ms, 0 = off (the daemon keeps it in /run) */
    int led;         /* LED state written, -1 = unknown */
    unsigned long ticks;
    unsigned gen;    /* bumped when something D-Bus shows as a property changes */
};

/* ---- config (fand.c) */
void cfg_defaults(struct config *c);
/* parses KEY=VALUE text over c; bad values are reported in err and keep
 * what c had. Returns the number of errors */
int cfg_parse(struct config *c, const char *text, char err[][128], int maxerr);
int cfg_load(struct config *c, const char *file, char err[][128], int maxerr);
int curve_parse(const char *s, struct curve *cv, double floor, char *err, size_t n);
double curve_eval(const struct curve *cv, double t);
void curve_format(const struct curve *cv, char *buf, size_t n);
/* replaces (or adds) KEY=VALUE in the file, atomically */
int cfg_save_key(const char *file, const char *key, const char *value);
extern const char *const group_id[NGROUPS], *const zone_id[NZONES], *const mode_name[3];
const char *curve_key(int zone, int group);

/* ---- control (fand.c) */
void fand_init(struct fand *f, const struct config *c, int64_t now);
/* applies a new config, keeping the state */
void fand_reconfig(struct fand *f, const struct config *c);
void fand_step(struct fand *f, int64_t now, int host);
/* every configured fan to 100 %, as fast as possible (exit, crash) */
void fand_failsafe(const struct config *c);
/* 1: a platform device answers (the platform is powered) */
int fand_probe(const struct config *c);
int fand_set_mode(struct fand *f, int zone, int mode, char *err, size_t n);
int fand_set_duty(struct fand *f, int zone, double duty, char *err, size_t n);
int fand_confirm(struct fand *f, int zone, char *err, size_t n);
int fand_set_curve(struct fand *f, int zone, int group, const char *pts, char *err, size_t n);
void fand_clear_alarms(struct fand *f);
/* minutes: 0 off, 1..FAND_TURBO_MAX_MIN, FAND_TURBO_UNTIL_OFF */
int fand_set_turbo(struct fand *f, unsigned minutes, char *err, size_t n);
int fand_turbo_left(const struct fand *f); /* seconds, 0 = off */
int fand_alarm(const struct fand *f);
void fand_alarm_text(const struct fand *f, char *buf, size_t n);
const char *fand_host_name(const struct fand *f);
int zone_by_name(const char *s);
/* a fan's duty for a zone duty (tests) */
double fan_duty_test(const struct output *o, double floor, double z);
int group_by_name(const char *s);
int mode_by_name(const char *s);

/* growable string for the JSON */
struct buf
{
    char *s;
    size_t len, cap;
    int oom;
};
void buf_printf(struct buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void fand_json(const struct fand *f, struct buf *b);

/* observe only: everything is read and computed, nothing is written (no
 * fan, no LED, no fail-safe) */
extern int fand_observe;

/* ---- logging: to stderr (the journal), with a syslog priority prefix */
extern int fand_quiet;
void fand_log(int prio, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* ---- hardware (fand-hw.c). REIMU_FAND_FAKE=<dir>: devices are register
 * files <dir>/i2c-<bus>/<addr, 2 hex digits> (256 bytes; a plain 2-byte
 * read gives bytes 0, 1), the LED <dir>/leds/<name>/{trigger,brightness} */
int hw_rd_byte(int bus, int addr, int reg);
int hw_wr_byte(int bus, int addr, int reg, int val);
int hw_rd_word(int bus, int addr, int reg);
int hw_rd_plain2(int bus, int addr, uint8_t b[2]);
int hw_led(const char *name, int on);
unsigned long hw_transfers(void); /* I2C transfers so far (tests) */
void hw_reset(void);              /* forget cached fds and env (tests) */

#endif
