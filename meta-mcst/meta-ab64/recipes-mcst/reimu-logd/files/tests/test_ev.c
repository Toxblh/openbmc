/* native tests of the event log's logic: cc -I.. tests/test_ev.c ev-core.c */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ev-core.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static char path[256], old[260];
static size_t fsize(const char *p) { struct stat st; return stat(p, &st) ? 0 : st.st_size; }
static char *slurp(const char *p)
{
    static char b[1 << 20];
    FILE *f = fopen(p, "r");
    size_t n = f ? fread(b, 1, sizeof(b) - 1, f) : 0;
    b[n] = 0;
    if (f) fclose(f);
    return b;
}
static void reset(struct ev *e) { unlink(path); unlink(old); ev_init(e, path); }

int main(void)
{
    static struct ev e;
    snprintf(path, sizeof(path), "/tmp/ev-test-%d.log", getpid());
    snprintf(old, sizeof(old), "%s.1", path);

    /* batching: one write a minute however many lines */
    reset(&e);
    int64_t t = 1000;
    ev_add(&e, t, 1, 4, "a", 1, "first", 0);
    CHECK(ev_flush_due(&e, t), "the first flush at once");
    ev_flush(&e, t);
    CHECK(e.writes == 1, "writes %lu", e.writes);
    ev_add(&e, t + 1, 2, 4, "a", 1, "second", 0);
    CHECK(!ev_flush_due(&e, t + 30), "not within the minute");
    CHECK(ev_flush_due(&e, t + 60), "after the minute");
    /* critical: within 5 s */
    ev_add(&e, t + 2, 3, 2, "a", 1, "crit", 1);
    CHECK(!ev_flush_due(&e, t + 4) && ev_flush_due(&e, t + 5), "critical after 5 s");
    ev_flush(&e, t + 5);
    CHECK(strstr(slurp(path), "second") && strstr(slurp(path), "crit"), "both written");

    /* repeats counted */
    reset(&e);
    ev_add(&e, t, 1, 4, "a", 1, "same", 0);
    for (int i = 0; i < 50; i++) ev_add(&e, t, 1, 4, "a", 1, "same", 0);
    ev_add(&e, t, 1, 4, "a", 1, "other", 0);
    ev_flush(&e, t);
    CHECK(strstr(slurp(path), "50 more times"), "%s", slurp(path));

    /* a source's lines a minute */
    reset(&e);
    int taken = 0;
    char m[32];
    for (int i = 0; i < 100; i++) { snprintf(m, sizeof(m), "line %d", i); taken += ev_add(&e, t, 1, 4, "spam", 1, m, 0); }
    CHECK(taken == EV_SRC_LINES, "taken %d", taken);
    CHECK(ev_add(&e, t, 1, 4, "other", 1, "still", 0) == 1, "another source passes");
    ev_flush(&e, t);
    CHECK(strstr(slurp(path), "dropped 80 events"), "%s", slurp(path));

    /* the minute's and the hour's bytes; a critical line passes them */
    reset(&e);
    char big[250];
    memset(big, 'x', sizeof(big) - 8);
    big[sizeof(big) - 8] = 0;
    size_t total = 0;
    for (int min = 0; min < 180; min++)
    {
        int64_t now = t + min * 60;
        for (int i = 0; i < 200; i++)
        {
            char id[16], msg[300];
            snprintf(id, sizeof(id), "s%d", i % 10);
            snprintf(msg, sizeof(msg), "%s %d %d", big, min, i);
            ev_add(&e, now, now, 4, id, 1, msg, 0);
        }
        ev_flush(&e, now);
        total = fsize(path) + fsize(old);
        if (min == 0) CHECK(total <= EV_MIN_BYTES + 200, "a minute: %zu", total);
    }
    CHECK(total <= 4 * EV_HOUR_BYTES + 180 * 80, "3 hours of flood (4 hour windows, a note a minute): %zu bytes", total);
    CHECK(e.writes <= 180, "writes %lu", e.writes);
    CHECK(ev_add(&e, t + 180 * 60, 1, 1, "x", 1, "alert", 1) == 1, "critical passes the hour's limit");

    /* rotation, bounded */
    reset(&e);
    FILE *f = fopen(path, "w");
    for (int i = 0; i < EV_FILE_MAX - 20; i++) fputc(i % 60 ? 'p' : '\n', f);
    fputc('\n', f);
    fclose(f);
    ev_add(&e, t, 1, 4, "a", 1, "new line in the new file", 0);
    ev_flush(&e, t);
    CHECK(fsize(old) > 0 && fsize(path) < 200 && strstr(slurp(path), "new line"), "rotated: %zu %zu", fsize(path), fsize(old));

    /* a torn last line */
    reset(&e);
    f = fopen(path, "w");
    fputs("2026 whole\ntorn wit", f);
    fclose(f);
    ev_add(&e, t, 1, 4, "a", 1, "next", 0);
    ev_flush(&e, t);
    CHECK(strstr(slurp(path), "torn wit\n") != NULL, "%s", slurp(path));

    /* long lines cut, control characters blanked */
    reset(&e);
    char lng[1000];
    memset(lng, 'y', sizeof(lng));
    lng[999] = 0;
    lng[3] = '\n';
    ev_add(&e, t, 1, 4, "a", 1, lng, 0);
    ev_flush(&e, t);
    CHECK(fsize(path) <= EV_LINE_MAX + 1 && strchr(slurp(path), '\n') == slurp(path) + fsize(path) - 1, "one line of %zu", fsize(path));

    /* a failed write keeps the batch */
    reset(&e);
    ev_init(&e, "/nonexistent-dir/x.log");
    ev_add(&e, t, 1, 4, "a", 1, "kept", 0);
    CHECK(ev_flush(&e, t) == -1 && e.npend > 0 && e.errors == 1, "kept");

    /* clean stop */
    reset(&e);
    CHECK(!ev_prev_clean(path), "no file: not clean");
    ev_add_note(&e, 1, 5, EV_STOP_TEXT);
    ev_flush(&e, t);
    CHECK(ev_prev_clean(path), "clean");
    ev_add(&e, t + 100, 1, 4, "a", 1, "after", 0);
    ev_flush(&e, t + 100);
    CHECK(!ev_prev_clean(path), "a line after it: not clean");

    /* reset reasons */
    CHECK(!strcmp(ev_reset_reason(0x20, 1, 1, "x"), "reboot"), "reboot");
    CHECK(!strcmp(ev_reset_reason(0x20, 5, 0, "x"), "watchdog"), "watchdog");
    CHECK(!strcmp(ev_reset_reason(0, 1, 0, "x"), "power-on"), "power-on");
    CHECK(!strcmp(ev_reset_reason(0, 1, 1, "x"), "kexec-or-reboot"), "a clean stop, no flag");
    CHECK(!strcmp(ev_reset_reason(0x20, 1, 0, "a reimu.ram b"), "kexec"), "kexec by cmdline");
    CHECK(!strcmp(ev_reset_reason(0, 2, 0, "x"), "external-reset"), "external");

    unlink(path);
    unlink(old);
    printf("%d failed\n", fails);
    return fails != 0;
}
