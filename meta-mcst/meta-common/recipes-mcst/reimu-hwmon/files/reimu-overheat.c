/*
 * The overheat protection for the web UI: D-Bus object
 * /xyz/openbmc_project/reimu/overheat of reimu-hwmon.
 *
 * overheatd itself has no D-Bus (the last resort depends on nothing): it reads
 * its config on every poll and writes its state to a file. Here:
 *   Json              the power actions on or off, the temperatures in force,
 *                     the ladder's state, the reason of an emergency power-off
 *   SetPowerOff(b)    OVERHEAT_POWEROFF in /etc/overheatd.conf (rwfs: kept over
 *                     reboots and A/B updates); overheatd takes it on its next
 *                     poll. bmcweb's REST lets only an admin call a method.
 *   ClearEmergency()  the marker of an emergency power-off, without powering on
 *
 * Copyright (c) 2026 MCST REIMU contributors. Licensed under GPLv2.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <systemd/sd-bus.h>
#include "rbuf.h"
#include "reimu-overheat.h"

#define CONF "/etc/overheatd.conf"
#define STATE "/var/volatile/systemstate.xml"
#define MARKER "/var/lib/overheatd/emergency"
#define OBJ "/xyz/openbmc_project/reimu/overheat"
#define IFACE "xyz.openbmc_project.Reimu.Overheat"
/* as overheatd's when it has to create the file */
#define DEFAULT_CONF "OVERHEATD_ENABLED=yes\nDEBUG=off\nCPU_TEMP_LIMIT=85\nWAIT_MSEC=5000\nBLINK_MSEC=500\n" \
                     "CPU_SHUTDOWN_TEMP=90\nCPU_POWEROFF_TEMP=95\nGRACEFUL_TIMEOUT_SEC=60\n"

static int read_all(const char *path, char *out, size_t size)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t n = 0;
    ssize_t r;
    while (n < size - 1 && (r = read(fd, out + n, size - 1 - n)) > 0) n += r;
    close(fd);
    out[n] = 0;
    return (int)n;
}

/* OVERHEAT_POWEROFF=no in the config: anything else is overheatd's default, on */
static int conf_poweroff(const char *conf)
{
    const char *p = strstr(conf, "OVERHEAT_POWEROFF=");
    return !(p && (p == conf || p[-1] == '\n') && !strncmp(p + 18, "no\n", 3));
}

/* a value of overheatd's state file: <protection name="value" */
static void state_attr(struct buf *b, const char *state, const char *name, const char *json, int number)
{
    char key[32];
    snprintf(key, sizeof(key), " %s=\"", name);
    const char *prot = strstr(state, "<protection");
    const char *p = prot ? strstr(prot, key) : NULL;
    const char *e = p ? strchr(p + strlen(key), '"') : NULL;
    buf_printf(b, ",\"%s\":", json);
    if (!e) buf_str(b, "null");
    else if (number) buf_add(b, p + strlen(key), e - p - strlen(key));
    else buf_json(b, p + strlen(key), e - p - strlen(key));
}

/* the document of the object (and of the web UI's view) */
char *overheat_doc(void)
{
    static char conf[4096], state[8192], marker[1024];
    struct buf b = { 0 };
    if (read_all(CONF, conf, sizeof(conf)) < 0) conf[0] = 0;
    if (read_all(STATE, state, sizeof(state)) < 0) state[0] = 0;
    buf_printf(&b, "{\"version\":1,\"poweroff\":%s", conf_poweroff(conf) ? "true" : "false");
    state_attr(&b, state, "limit", "limit", 1);
    state_attr(&b, state, "shutdown", "shutdown", 1);
    state_attr(&b, state, "poweroff", "off", 1);
    state_attr(&b, state, "timeout", "timeout", 1);
    state_attr(&b, state, "state", "state", 0);
    const char *st = strstr(state, "<status>");
    const char *ste = st ? strstr(st, "</status>") : NULL;
    buf_str(&b, ",\"status\":");
    if (ste) buf_json(&b, st + 8, ste - st - 8);
    else buf_str(&b, "null");
    /* blind for how long (the state file): the page says it */
    const char *blind = strstr(state, "<blind sec=\"");
    long blind_sec = 0;
    int blind_alarm = 0;
    if (blind) sscanf(blind, "<blind sec=\"%ld\" alarm=\"%d\"", &blind_sec, &blind_alarm);
    buf_printf(&b, ",\"blind_sec\":%ld,\"blind_alarm\":%d", blind_sec, blind_alarm);
    buf_str(&b, ",\"emergency\":");
    int n = read_all(MARKER, marker, sizeof(marker));
    if (n >= 0) buf_json(&b, marker, n);
    else buf_str(&b, "null");
    buf_str(&b, "}");
    return b.s;
}

static int prop_json(sd_bus *bus, const char *path, const char *iface, const char *prop, sd_bus_message *reply,
                     void *ud, sd_bus_error *err)
{
    (void)bus; (void)path; (void)iface; (void)prop; (void)ud; (void)err;
    char *j = overheat_doc();
    int r = sd_bus_message_append(reply, "s", j ? j : "{}");
    free(j);
    return r;
}

/* the whole file, replaced at once: overheatd reads it on every poll */
static int write_conf(const char *conf, int on)
{
    struct buf b = { 0 };
    const char *key = "OVERHEAT_POWEROFF=";
    size_t klen = strlen(key);
    for (const char *l = conf; *l;)
    {
        const char *e = strchr(l, '\n');
        size_t n = e ? (size_t)(e - l + 1) : strlen(l);
        if (strncmp(l, key, klen))
        {
            buf_add(&b, l, n);
            if (!e) buf_str(&b, "\n");
        }
        l += n;
    }
    buf_printf(&b, "%s%s\n", key, on ? "yes" : "no");
    int rv = -1;
    int fd = open(CONF ".new", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd >= 0)
    {
        if (write(fd, b.s, b.len) == (ssize_t)b.len && !fsync(fd)) rv = 0;
        close(fd);
    }
    if (!rv) rv = rename(CONF ".new", CONF);
    free(b.s);
    return rv;
}

static int method_set(sd_bus_message *m, void *ud, sd_bus_error *err)
{
    (void)ud;
    int on;
    static char conf[4096];
    int r = sd_bus_message_read(m, "b", &on);
    if (r < 0) return r;
    if (read_all(CONF, conf, sizeof(conf)) <= 0) snprintf(conf, sizeof(conf), "%s", DEFAULT_CONF);
    if (write_conf(conf, on)) return sd_bus_error_setf(err, SD_BUS_ERROR_FAILED, "can't write " CONF ": %s", strerror(errno));
    /* (overheatd says it at critical level when it takes it) */
    fprintf(stderr, "OVERHEAT_POWEROFF=%s set through D-Bus\n", on ? "yes" : "no");
    sd_bus_emit_properties_changed(sd_bus_message_get_bus(m), OBJ, IFACE, "Json", NULL);
    return sd_bus_reply_method_return(m, "s", "ok");
}

static int method_clear(sd_bus_message *m, void *ud, sd_bus_error *err)
{
    (void)ud;
    if (unlink(MARKER) && errno != ENOENT) return sd_bus_error_setf(err, SD_BUS_ERROR_FAILED, "can't remove " MARKER ": %s", strerror(errno));
    fprintf(stderr, "The emergency marker of overheatd cleared through D-Bus\n");
    sd_bus_emit_properties_changed(sd_bus_message_get_bus(m), OBJ, IFACE, "Json", NULL);
    return sd_bus_reply_method_return(m, "s", "ok");
}

static const sd_bus_vtable overheat_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("Json", "s", prop_json, 0, SD_BUS_VTABLE_PROPERTY_EMITS_INVALIDATION),
    SD_BUS_METHOD("SetPowerOff", "b", "s", method_set, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("ClearEmergency", "", "s", method_clear, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END
};

int overheat_register(sd_bus *bus)
{
    return sd_bus_add_object_vtable(bus, NULL, OBJ, IFACE, overheat_vtable, NULL);
}
