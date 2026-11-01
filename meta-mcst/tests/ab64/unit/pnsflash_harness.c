/* The D-Bus methods of reimu-pnsflash.c (the real file, included) as a command
 * line, without a bus (test_pns_tool.py):
 *
 *   pnsflash_harness run ACTION ARG | upload OFFSET DATA | json
 *
 * The reply goes to stdout. systemctl and systemd-run, whose paths the file has
 * fixed, are the programs in H_SYSTEMCTL and H_SYSTEMD_RUN; the host's state is
 * H_HOST (default Off). */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int harness_execv(const char *path, char *const argv[])
{
    const char *p = path;
    if (!strcmp(path, "/bin/systemctl")) p = getenv("H_SYSTEMCTL");
    if (!strcmp(path, "/usr/bin/systemd-run")) p = getenv("H_SYSTEMD_RUN");
    return execv(p ? p : path, argv);
}
#define execv harness_execv
#include "reimu-pnsflash.c"
#undef execv

struct sd_bus_message { char **args; };

int sd_bus_message_read(sd_bus_message *m, const char *types, ...)
{
    va_list ap;
    va_start(ap, types);
    for (; *types; types++, m->args++)
    {
        if (*types == 's') *va_arg(ap, const char **) = m->args[0];
        else if (*types == 't') *va_arg(ap, uint64_t *) = strtoull(m->args[0], NULL, 0);
        else *va_arg(ap, uint32_t *) = strtoul(m->args[0], NULL, 0);
    }
    va_end(ap);
    return 1;
}

int sd_bus_reply_method_return(sd_bus_message *m, const char *types, ...)
{
    (void)m; (void)types;
    va_list ap;
    va_start(ap, types);
    puts(va_arg(ap, const char *));
    va_end(ap);
    return 1;
}

int sd_bus_message_append(sd_bus_message *m, const char *types, ...) { (void)m; (void)types; return 1; }
sd_bus *sd_bus_message_get_bus(sd_bus_message *m) { (void)m; return NULL; }
int sd_bus_add_object_vtable(sd_bus *bus, void *slot, const char *path, const char *iface, const sd_bus_vtable *v, void *ud)
{
    (void)bus; (void)slot; (void)path; (void)iface; (void)v; (void)ud;
    return 0;
}

int sd_bus_get_property_string(sd_bus *bus, const char *dest, const char *path, const char *iface, const char *member,
                               sd_bus_error *err, char **out)
{
    (void)bus; (void)dest; (void)path; (void)iface; (void)member; (void)err;
    const char *h = getenv("H_HOST");
    *out = strdup(h ? h : "xyz.openbmc_project.State.Host.HostState.Off");
    return 0;
}

/* (in reimu-sensors.c, which needs a real bus) */
void buf_add(struct buf *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap)
    {
        b->cap = (b->len + n + 1) * 2;
        b->s = realloc(b->s, b->cap);
    }
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = 0;
}
void buf_str(struct buf *b, const char *s) { buf_add(b, s, strlen(s)); }
void buf_printf(struct buf *b, const char *fmt, ...)
{
    char t[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t, sizeof(t), fmt, ap);
    va_end(ap);
    buf_str(b, t);
}
void buf_json(struct buf *b, const char *s, size_t n)
{
    buf_str(b, "\"");
    for (size_t i = 0; i < n; i++)
    {
        if (s[i] == '"' || s[i] == '\\') buf_add(b, "\\", 1);
        if ((unsigned char)s[i] >= 0x20) buf_add(b, s + i, 1);
    }
    buf_str(b, "\"");
}

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    if (!strcmp(argv[1], "json"))
    {
        puts(pf_json());
        return 0;
    }
    if (argc < 4) return 2;
    struct sd_bus_message m = { argv + 2 };
    return (!strcmp(argv[1], "run") ? method_run : method_upload)(&m, NULL, NULL) < 0;
}
