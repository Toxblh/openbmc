/* reimu-hostlink without D-Bus, for test_disk.py: dbus_notify, a no-op in that
 * build, writes "STATE SECONDS" to the file REIMU_HOSTLINK_NOTIFY. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define dbus_notify dbus_notify_native
#include "hl-dbus.h"
#undef dbus_notify

static void dbus_notify(const char *state)
{
    const char *p = getenv("REIMU_HOSTLINK_NOTIFY");
    FILE *f = p ? fopen(p, "a") : NULL;
    struct timespec t;
    if (!f) return;
    clock_gettime(CLOCK_REALTIME, &t);
    fprintf(f, "%s %ld.%03ld\n", state, (long)t.tv_sec, t.tv_nsec / 1000000);
    fclose(f);
}

#include "reimu-hostlink.c"
