/* server_ctl.c against a fake platform (GPIO lines, simulated time; the PNS lock
 * is a real flock on a temporary file). Native, no hardware: test_static.py S71. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static char s_lockfile[64];
__attribute__((unused)) static int fake_clock(clockid_t c, struct timespec *ts);
#define clock_gettime(c, ts) fake_clock(c, ts)
#define system(x) 0
#define mkdir(p, m) ((void)0)
#define open(p, ...) open(s_lockfile, __VA_ARGS__)
#define main server_ctl_main
#include "server_ctl.c"
#undef main
#undef open

static long long s_ms;          /* simulated time */
static long long s_off_at;      /* power good drops then (-1: never) */
static long long s_err_at;      /* power good unreadable from then on (-1: never) */
static long long s_lock_free_at;
static int s_lock_fd = -1;
static int s_power;
static int s_on_by_press;       /* a press of a host that's off powers it up, at its end */
static int s_busy_reads;        /* the power good line busy for the raw reads of the hold */
static int s_npress, s_nreset;
static long long s_press_from, s_press_to;
static int s_driving, s_floated, s_polls, s_term_blocked;

static void tick(void)
{
    if (s_off_at >= 0 && s_ms >= s_off_at) s_power = 0;
    if (s_lock_fd >= 0 && s_ms >= s_lock_free_at)
    {
        close(s_lock_fd);
        s_lock_fd = -1;
    }
}

static int fake_clock(clockid_t c, struct timespec *ts)
{
    (void)c;
    ts->tv_sec = s_ms / 1000;
    ts->tv_nsec = (s_ms % 1000) * 1000000;
    return 0;
}

void reimu_msleep(long value, volatile int *req)
{
    (void)req;
    s_ms += value;
    tick();
}

static int power_read(void)
{
    if (s_err_at >= 0 && s_ms >= s_err_at) return -3;
    return s_power;
}

static void press_begins(void)
{
    sigset_t cur;
    sigprocmask(SIG_BLOCK, NULL, &cur);
    s_term_blocked = sigismember(&cur, SIGTERM);
    s_npress++;
    s_press_from = s_ms;
    s_driving = 1;
}

static void press_ends(void)
{
    s_press_to = s_ms;
    s_driving = 0;
    if (s_on_by_press) s_power = 1;
}

int reimu_get_gpio_by_name(const char *name)
{
    if (!strcmp(name, "GPIO_POWER_IN")) return power_read();
    return 0;
}

int reimu_set_gpio_by_name(const char *name, int val, int delay)
{
    (void)val;
    if (!strcmp(name, "GPIO_RST_BTN"))
    {
        s_nreset++;
        s_ms += delay;
        tick();
        return 0;
    }
    press_begins();
    s_ms += delay;
    tick();
    press_ends();
    s_floated = 1;
    return 0;
}

/* libreimu, not in its header */
int reimu_gpio_to_num(const char *name)
{
    return !strcmp(name, "GPIO_POWER_IN") ? 2 : !strcmp(name, "GPIO_PWR_BTN") ? 1 : 3;
}

/* libgpiod */
struct gpiod_chip { int x; };
struct gpiod_line { int num, held; };
static struct gpiod_chip s_chip;
static struct gpiod_line s_lines[4] = { { 0, 0 }, { 1, 0 }, { 2, 0 }, { 3, 0 } };
struct gpiod_chip *gpiod_chip_open(const char *p) { (void)p; return &s_chip; }
void gpiod_chip_close(struct gpiod_chip *c) { (void)c; }
struct gpiod_line *gpiod_chip_get_line(struct gpiod_chip *c, unsigned n) { (void)c; return &s_lines[n]; }
int gpiod_line_request_input(struct gpiod_line *l, const char *c)
{
    (void)c;
    if (l->num == 2 && (s_busy_reads || power_read() < 0))
    {
        s_polls++;
        errno = EBUSY;
        return -1;
    }
    if (l->num == 1 && !s_driving) s_floated = 1;
    l->held = 1;
    return 0;
}
int gpiod_line_request_output(struct gpiod_line *l, const char *c, int v)
{
    (void)c; (void)v;
    if (l->num == 1) press_begins();
    l->held = 1;
    return 0;
}
int gpiod_line_get_value(struct gpiod_line *l)
{
    if (l->num == 2) s_polls++;
    return l->num == 2 ? power_read() : 1;
}
void gpiod_line_release(struct gpiod_line *l)
{
    if (l->num == 1 && s_driving) press_ends();
    l->held = 0;
}

/* the rest of libreimu server_ctl links with: not reached here */
int reimu_dbus_set_property_bool(const char *a, const char *b, const char *c, const char *d, int e) { (void)a; (void)b; (void)c; (void)d; (void)e; return 0; }
int reimu_dbus_get_property_bool(const char *a, const char *b, const char *c, const char *d) { (void)a; (void)b; (void)c; (void)d; return 0; }
int reimu_dbus_call_method(const char *a, const char *b, const char *c, const char *d) { (void)a; (void)b; (void)c; (void)d; return 0; }
int reimu_find_in_file(const char *a, const char *b) { (void)a; (void)b; return 1; }
int reimu_chkdir(const char *a) { (void)a; return 1; }
int reimu_writefile(const char *a, const void *b, size_t c) { (void)a; (void)b; (void)c; return 0; }
int reimu_readfile(const char *a, char **b, size_t *c) { (void)a; (void)b; (void)c; return 1; }

static int s_fail, s_pass;
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

static void reset(int power)
{
    if (pns_fd >= 0) close(pns_fd);
    pns_fd = -1;
    if (s_lock_fd >= 0) close(s_lock_fd);
    s_lock_fd = -1;
    s_ms = 1000;
    s_off_at = s_err_at = -1;
    s_lock_free_at = 0;
    s_power = power;
    s_on_by_press = s_busy_reads = 0;
    s_npress = s_nreset = s_polls = s_floated = s_driving = 0;
    s_press_from = s_press_to = -1;
}

/* a flash job (or the previous press) has the lock until then */
static void lock_until(long long ms)
{
    s_lock_fd = open(s_lockfile, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (s_lock_fd < 0 || flock(s_lock_fd, LOCK_EX | LOCK_NB)) abort();
    s_lock_free_at = ms;
}

#define HELD (s_press_to - s_press_from)

int main(void)
{
    char *no[] = { "x", NULL };
    snprintf(s_lockfile, sizeof(s_lockfile), "%s/server-ctl-test-%d", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", getpid());

    /* the host goes off while the forced power-off waits for the lock */
    reset(1);
    lock_until(4000);
    s_off_at = 3900;
    int rv = server_pwr_off_hard(1, no);
    CHECK(s_npress == 0 && rv == 0, "hard off, off while waiting for the lock: no press (%d, rv %d)", s_npress, rv);

    reset(1);
    lock_until(4000);
    s_off_at = 3900;
    rv = server_pwr_off(1, no);
    CHECK(s_npress == 0 && rv == 0, "soft off, off while waiting for the lock: no press (%d, rv %d)", s_npress, rv);

    /* the same, power good unreadable by then: the button could power it on */
    reset(1);
    lock_until(4000);
    s_err_at = 3900;
    rv = server_pwr_off_hard(1, no);
    CHECK(s_npress == 0 && rv < 0, "power good unknown after the lock: no press, an error (%d, rv %d)", s_npress, rv);

    /* still on after the wait: pressed */
    reset(1);
    lock_until(4000);
    rv = server_pwr_off_hard(1, no);
    CHECK(s_npress == 1 && rv == 0 && s_press_from >= 4000, "hard off, on after the lock: pressed");

    /* power good drops during the hold: released at it */
    reset(1);
    s_off_at = 1000 + 4000;
    rv = server_pwr_off_hard(1, no);
    CHECK(s_npress == 1 && rv == 0 && HELD >= 4000 && HELD <= 4200, "drop at 4 s: released within a poll (held %lld)", HELD);
    CHECK(s_floated && !s_driving, "drop at 4 s: the line floats after it");
    CHECK(s_term_blocked, "the press holds SIGTERM off");

    /* power good doesn't drop: the same limit as ever, polled meanwhile */
    reset(1);
    rv = server_pwr_off_hard(1, no);
    CHECK(s_npress == 1 && rv == 0 && HELD == 5000, "no drop: 5 s exactly (held %lld)", HELD);
    CHECK(s_polls >= 20 && s_polls <= 26, "no drop: power good polled every 200 ms (%d)", s_polls);
    CHECK(s_floated && !s_driving, "no drop: the line floats after it");

    /* unreadable during the hold: not a drop */
    reset(1);
    s_busy_reads = 1;
    rv = server_pwr_off_hard(1, no);
    CHECK(s_npress == 1 && HELD == 5000, "power good busy during the hold: 5 s (held %lld)", HELD);
    reset(1);
    s_err_at = 2000;
    rv = server_pwr_off_hard(1, no);
    CHECK(s_npress == 1 && HELD == 5000, "power good failing during the hold: 5 s (held %lld)", HELD);

    /* the others as ever */
    reset(1);
    s_off_at = 1100;
    rv = server_pwr_off(1, no);
    CHECK(s_npress == 1 && HELD == 200 && rv == 0, "soft off: 200 ms whole (%d, held %lld)", s_npress, HELD);
    reset(0);
    s_on_by_press = 1;
    rv = server_pwr_on(1, no);
    CHECK(s_npress == 1 && HELD == 200 && rv == 0, "power on: 200 ms (%d, held %lld)", s_npress, HELD);
    reset(1);
    rv = server_pwr_on(1, no);
    CHECK(s_npress == 0 && rv == 0, "power on, on already: no press");
    reset(0);
    rv = server_pwr_off_hard(1, no);
    CHECK(s_npress == 0 && rv == 0, "hard off, off already: no press");
    reset(0);
    s_on_by_press = 1;
    rv = server_pwrbut_s(1, no);
    CHECK(s_npress == 1 && HELD == 200, "the raw short press: pressed on a host that is off");
    reset(0);
    s_on_by_press = 1;
    rv = server_pwrbut_h(1, no);
    CHECK(s_npress == 1 && HELD == 5000, "the raw long press of a host that is off: whole (held %lld)", HELD);
    reset(1);
    rv = server_reset(1, no);
    CHECK(s_nreset == 1 && s_npress == 0 && rv == 0, "reset: its own line");

    unlink(s_lockfile);
    printf("%d checks, %d failed\n", s_pass + s_fail, s_fail);
    return s_fail ? 1 : 0;
}
