/* overheatd.c's power ladder (ladder_step, action_start) against a fake platform:
 * no children, simulated time. Native, no hardware: test_static.py S72, S73. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static long s_t = 1000;
static int s_forks, s_pids, s_child_runs;
static int s_btn = 0;
static int fake_clock(clockid_t c, struct timespec *ts) { (void)c; ts->tv_sec = s_t; ts->tv_nsec = 0; return 0; }
static pid_t fake_waitpid(pid_t p, int *st, int fl)
{
    if ((fl & WNOHANG) && s_child_runs) return 0;
    if (st) *st = 0;
    return p;
}
#define clock_gettime(c, ts) fake_clock(c, ts)
#define fork() (++s_pids + 100000)
#define waitpid(p, st, fl) fake_waitpid(p, st, fl)
#define kill(p, s) 0
#define open(...) (-1)
#define socket(...) (-1)
#define mkdir(p, m) ((void)0)
#define unlink(p) ((void)0)
#define main overheatd_main
#include "overheatd.c"
#undef main
#define s_polls_reset() (s_known_polls = 0)

int reimu_get_gpio_by_name(const char *name) { return !strcmp(name, "GPIO_PWR_BTN") ? s_btn : 1; }
void reimu_message(FILE *s, const char *f, ...) { (void)s; (void)f; }
void reimu_cancel(int n, const char *f, ...) { (void)f; exit(n); }
char *reimu_gettime(void) { static char s[] = "t"; return s; }
void reimu_msleep(long v, volatile int *r) { (void)v; (void)r; }
int reimu_writefile(const char *a, const void *b, size_t c) { (void)a; (void)b; (void)c; return 0; }
int reimu_chkdir(const char *a) { (void)a; return 1; }
int reimu_chkfile(const char *a) { (void)a; return 1; }
int reimu_find_filename(int d, char *a, char **b) { (void)d; (void)a; (void)b; return 1; }
int reimu_find_in_file(const char *a, const char *b) { (void)a; (void)b; return 1; }
int reimu_get_conf_bool(const char *a, int d, const char *b) { (void)a; (void)d; (void)b; return 0; }
long reimu_get_conf_long(const char *a, int d, const char *b) { (void)a; (void)d; (void)b; return 0; }
void reimu_set_atexit(int *a, void (*f)(void)) { (void)a; (void)f; }
int reimu_textfile_buf_alloc(void) { return 0; }
int reimu_textfile_buf_append(const char *f, ...) { (void)f; return 0; }
int reimu_textfile_buf_commit(const char *a) { (void)a; return 0; }

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

/* one poll, a second later */
static void step(int power, int known, int shutdown)
{
    s_t++;
    pid_t before = s_action_pid;
    ladder_step(power, known, shutdown, 0, 0, "t");
    if (s_action_pid && s_action_pid != before) s_forks++; /* (log_entry forks too) */
}

static void fresh(int state)
{
    s_forks = s_child_runs = 0;
    s_btn = 0;
    s_ladder = LADDER_IDLE;
    step(0, 0, 0); /* forgets the polls, whatever the last case left */
    s_ladder = state;
    s_action_pid = 0;
    s_action_rv = 0;
    s_action_done = 0;
    s_action_force = 0;
    s_graceful_cooled = 0;
    s_polls_reset();
}

int main(void)
{
    /* a restart 40 s into a graceful shutdown of a hung host at 92 C: 30 s of settle
     * read nothing, then the readings count up to confirmed */
    fresh(LADDER_GRACEFUL);
    s_graceful_since = s_t - 40;
    s_action_rv = ACTION_SENT;
    for (int i = 0; i < 30; i++)
    {
        step(1, 0, 0);
        CHECK(!s_graceful_cooled, "no reading, %d s after the restart: not cooled", i + 1);
        CHECK(s_ladder == LADDER_GRACEFUL, "no reading, %d s after the restart: goes on (%s)", i + 1, s_ladder_names[s_ladder]);
    }
    step(1, 1, 0);
    step(1, 1, 0);
    CHECK(!s_graceful_cooled && s_ladder == LADDER_GRACEFUL, "the first readings, not yet confirmed: not cooled");
    step(1, 1, 1);
    CHECK(s_ladder == LADDER_FORCING && s_forks == 1 && s_action_force == 1, "confirmed hot past the timeout: forced at once, no second graceful press (%s, %d, force %d)",
          s_ladder_names[s_ladder], s_forks, s_action_force);

    /* the jump filter and the confirmation together: up to 6 polls of real readings */
    fresh(LADDER_GRACEFUL);
    s_graceful_since = s_t;
    s_action_rv = ACTION_SENT;
    for (int i = 0; i < 6; i++) step(1, 1, 0);
    CHECK(!s_graceful_cooled, "6 polls of readings: too few to say cooled");
    step(1, 1, 0);
    CHECK(s_graceful_cooled && s_ladder == LADDER_GRACEFUL, "7 polls under the limit: cooled, the state kept until the timeout");
    s_t += graceful_timeout_sec;
    step(1, 1, 0);
    CHECK(s_ladder == LADDER_IDLE && s_forks == 0, "cooled for real, the timeout over: idle, no press");

    /* a reading lost in between: counted anew */
    fresh(LADDER_GRACEFUL);
    s_graceful_since = s_t;
    s_action_rv = ACTION_SENT;
    for (int i = 0; i < 6; i++) step(1, 1, 0);
    step(1, 0, 0);
    for (int i = 0; i < 6; i++) step(1, 1, 0);
    CHECK(!s_graceful_cooled, "the readings stopped in between: counted anew");

    /* blind for long: nothing decided, in either direction */
    fresh(LADDER_GRACEFUL);
    s_graceful_since = s_t;
    s_action_rv = ACTION_SENT;
    for (int i = 0; i < 300; i++) step(1, 0, 0);
    CHECK(s_ladder == LADDER_GRACEFUL && s_forks == 0 && !s_graceful_cooled, "blind: no decision, no press");
    step(0, 0, 0);
    CHECK(s_ladder == LADDER_IDLE, "blind, the host goes off: idle");

    /* a restart during a forced press: its line is busy */
    fresh(LADDER_FORCING);
    s_btn = -3;
    button_release();
    for (int i = 0; i < 4; i++)
    {
        step(1, 1, 1);
        CHECK(s_forks == 0, "the press of the last run still on (%d s): no action", i + 1);
    }
    s_btn = 0;
    step(0, 1, 1);
    CHECK(s_ladder == LADDER_IDLE && s_forks == 0, "it ended, the host is off: no second press");

    /* ... that didn't power it off: pressed again, once */
    fresh(LADDER_FORCING);
    s_btn = -3;
    button_release();
    for (int i = 0; i < 4; i++) step(1, 1, 1);
    s_btn = 0;
    for (int i = 0; i < 3; i++) step(1, 1, 1);
    CHECK(s_forks == 1 && s_action_force == 1, "it ended, the host still on: pressed (%d)", s_forks);

    /* not stopped for good by a line that stays busy */
    fresh(LADDER_FORCING);
    s_btn = -3;
    button_release();
    for (int i = 0; i < ACTION_TIMEOUT_SEC + 5; i++) step(1, 1, 1);
    CHECK(s_forks >= 1, "busy for good: tried after ACTION_TIMEOUT_SEC anyway (%d)", s_forks);

    /* a free line: at once, as ever */
    fresh(LADDER_FORCING);
    button_release();
    step(1, 1, 1);
    CHECK(s_forks == 1, "restart, no press on: forcing goes on at the first poll (%d)", s_forks);

    /* a graceful request that had to wait for the line isn't lost */
    fresh(LADDER_IDLE);
    s_btn = -3;
    button_release();
    step(1, 1, 1);
    CHECK(s_ladder == LADDER_GRACEFUL && s_forks == 0, "over the limit, the line busy: graceful, not pressed yet");
    s_btn = 0;
    for (int i = 0; i < 6; i++) step(1, 1, 1);
    CHECK(s_forks == 1 && s_action_force == 0, "the line free: the graceful request made, once (%d)", s_forks);

    printf("%d checks, %d failed\n", s_pass + s_fail, s_fail);
    return s_fail ? 1 : 0;
}
