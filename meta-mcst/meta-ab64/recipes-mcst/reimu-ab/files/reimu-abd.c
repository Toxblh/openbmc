/*
 * reimu-abd: BMC firmware updates through Redfish UpdateService (and the
 * web UI) on the A/B image, in place of phosphor-bmc-code-mgmt, whose
 * static-layout updater would write partitions this layout doesn't have.
 *
 * bmcweb stores an uploaded image in /tmp/images and waits for an object
 * with xyz.openbmc_project.Software.Activation under
 * /xyz/openbmc_project/software, sets its RequestedActivation to Active and
 * follows Activation / ActivationProgress as a Redfish task. Here:
 *
 *   - an image that is a reimu-ab update (a tar of image-kernel, image-rofs,
 *     SHA256SUMS; the version in the rofs trailer) becomes such an object
 *     (Ready); anything else is deleted and reported (a phosphor-logging
 *     entry ImageFailure, which bmcweb turns into a 400 at once);
 *   - RequestedActivation = Active runs "reimu-ab install IMAGE": the slot
 *     that isn't running, every write read back, then that slot boots next
 *     (pending until healthy). Activation goes Activating -> Active (the
 *     new firmware runs after a BMC reboot) or Failed. The image is deleted
 *     afterwards;
 *   - the running firmware is an Active, functional object as before
 *     (Managers/bmc FirmwareVersion, FirmwareInventory).
 *
 * Status for the web UI: /xyz/openbmc_project/reimu/ab,
 * xyz.openbmc_project.Reimu.Ab, property Json:
 *   {"booted", "next", "pending", "tries", "slots": {"a", "b"}, "running",
 *    "job": {"state": running|ok|failed, "progress", "message", "version",
 *            "started", "finished"}, "upload": {"ok", "message", "time"}}
 *   method Refresh() -> s   re-read the slots ("reimu-ab status")
 *
 * Copyright (c) 2026 MCST REIMU contributors. Licensed under GPLv2.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>

#define BUSNAME   "xyz.openbmc_project.Software.BMC.Updater"
#define SW        "/xyz/openbmc_project/software"
#define IMAGES    "/tmp/images"
#define ACT       "xyz.openbmc_project.Software.Activation.Activations."
#define REQ       "xyz.openbmc_project.Software.Activation.RequestedActivations."
#define PURPOSE   "xyz.openbmc_project.Software.Version.VersionPurpose.BMC"
#define AB_PATH   "/xyz/openbmc_project/reimu/ab"
#define AB_IFACE  "xyz.openbmc_project.Reimu.Ab"

static const char *reimu_ab(void)
{
    const char *e = getenv("REIMU_AB");
    return e && *e ? e : "/usr/sbin/reimu-ab";
}

static sd_bus *bus;
static sd_event *ev;

/* ------------------------------------------------------------------ */
/* small growable string                                               */

struct buf
{
    char *s;
    size_t len, cap;
};

static void badd(struct buf *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap)
    {
        size_t cap = b->cap ? b->cap : 1024;
        while (cap < b->len + n + 1) cap *= 2;
        char *ns = realloc(b->s, cap);
        if (!ns) return;
        b->s = ns;
        b->cap = cap;
    }
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = 0;
}

static void bprintf(struct buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void bprintf(struct buf *b, const char *fmt, ...)
{
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) badd(b, tmp, (size_t)n < sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1);
}

/* JSON string; control characters dropped (the text is ours or reimu-ab's) */
static void bjson(struct buf *b, const char *s)
{
    badd(b, "\"", 1);
    for (; s && *s; s++)
    {
        unsigned char c = *s;
        if (c == '"' || c == '\\')
        {
            char e[2] = { '\\', (char)c };
            badd(b, e, 2);
        }
        else if (c >= 0x20 && c != 0x7f) badd(b, s, 1);
    }
    badd(b, "\"", 1);
}

/* ------------------------------------------------------------------ */
/* images                                                              */

struct image
{
    char id[16], path[256], version[128], obj[96];
    const char *activation, *requested; /* suffixes of ACT, REQ */
    uint8_t progress;
    sd_bus_slot *slot;
};

static struct image running = { .activation = "Active", .requested = "None" };
static struct image upload; /* id[0] == 0: none */

/* the job (an install) and the last upload, for the web UI */
static struct
{
    const char *state; /* NULL: none yet */
    int progress;
    char message[512], version[128];
    time_t started, finished;
    pid_t pid;
    struct buf out;
} job;
static struct
{
    int ok;
    char message[512];
    time_t time;
} last_upload;

/* the slots ("reimu-ab status") */
static struct
{
    char booted[128], next[8], tries[8], a[128], b[128];
    int pending;
    time_t read;
} slots;

static void make_id(const char *s, char out[16])
{
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
    snprintf(out, 16, "%08x", h);
}

static void os_version(char *out, size_t sz)
{
    FILE *f = fopen("/etc/os-release", "r");
    char l[256];
    snprintf(out, sz, "unknown");
    if (!f) return;
    while (fgets(l, sizeof(l), f))
        if (!strncmp(l, "VERSION_ID=", 11))
        {
            char *v = l + 11;
            v[strcspn(v, "\n")] = 0;
            if (*v == '"') v++;
            size_t n = strlen(v);
            if (n && v[n - 1] == '"') v[n - 1] = 0;
            snprintf(out, sz, "%s", v);
        }
    fclose(f);
}

static void read_slots(void)
{
    char cmd[300];
    snprintf(cmd, sizeof(cmd), "%s status 2>/dev/null", reimu_ab());
    FILE *p = popen(cmd, "r");
    char l[512];
    memset(&slots, 0, sizeof(slots));
    slots.read = time(NULL);
    if (!p) return;
    while (fgets(l, sizeof(l), p))
    {
        l[strcspn(l, "\n")] = 0;
        if (!strncmp(l, "booted: ", 8)) snprintf(slots.booted, sizeof(slots.booted), "%s", l + 8);
        else if (!strncmp(l, "next: slot ", 11))
        {
            snprintf(slots.next, sizeof(slots.next), "%.1s", l + 11);
            char *t = strstr(l, "mark-good: ");
            if (t) snprintf(slots.tries, sizeof(slots.tries), "%.*s", (int)strspn(t + 11, "0123456789"), t + 11);
            slots.pending = strstr(l, "pending") != NULL;
        }
        else if (!strncmp(l, "slot a: ", 8)) snprintf(slots.a, sizeof(slots.a), "%s", l + 8);
        else if (!strncmp(l, "slot b: ", 8)) snprintf(slots.b, sizeof(slots.b), "%s", l + 8);
    }
    pclose(p);
}

static void emit(struct image *im, const char *iface, const char *prop)
{
    if (im->id[0]) sd_bus_emit_properties_changed(bus, im->obj, iface, prop, NULL);
}

static void set_activation(struct image *im, const char *a)
{
    im->activation = a;
    emit(im, "xyz.openbmc_project.Software.Activation", "Activation");
}

static void set_progress(struct image *im, int p)
{
    if (p <= im->progress) return;
    im->progress = p;
    job.progress = p;
    emit(im, "xyz.openbmc_project.Software.ActivationProgress", "Progress");
}

/* bmcweb answers a pending upload at once (400) when it sees an
 * xyz.openbmc_project.Logging.Entry Message "...Software.Image.Error.X"
 * change under /xyz/openbmc_project/logging/entry: sent here directly (the
 * entry phosphor-logging creates below doesn't always signal that way) */
static void tell_bmcweb(const char *message)
{
    sd_bus_message *m = NULL;
    if (sd_bus_message_new_signal(bus, &m, "/xyz/openbmc_project/logging/entry/reimu_abd",
                                  "org.freedesktop.DBus.Properties", "PropertiesChanged") < 0)
        return;
    sd_bus_message_append(m, "sa{sv}as", "xyz.openbmc_project.Logging.Entry", 1, "Message", "s", message, 0);
    sd_bus_send(bus, m, NULL);
    sd_bus_message_unref(m);
}

/* a phosphor-logging entry (the event log), and bmcweb told */
static void log_error(const char *message, const char *reason, const char *path)
{
    sd_bus_message *m = NULL;
    tell_bmcweb(message);
    if (sd_bus_message_new_method_call(bus, &m, "xyz.openbmc_project.Logging", "/xyz/openbmc_project/logging",
                                       "xyz.openbmc_project.Logging.Create", "Create") < 0)
        return;
    sd_bus_message_append(m, "ss", message, "xyz.openbmc_project.Logging.Entry.Level.Error");
    sd_bus_message_append(m, "a{ss}", 2, "PATH", path, "REASON", reason);
    sd_bus_call_async(bus, NULL, m, NULL, NULL, 0);
    sd_bus_message_unref(m);
}

/* ------------------------------------------------------------------ */
/* the D-Bus objects                                                   */

static int p_act(sd_bus *b, const char *path, const char *iface, const char *prop, sd_bus_message *r, void *ud,
                 sd_bus_error *e)
{
    (void)b; (void)path; (void)iface; (void)e;
    struct image *im = ud;
    char s[160];
    if (!strcmp(prop, "Activation")) snprintf(s, sizeof(s), ACT "%s", im->activation);
    else snprintf(s, sizeof(s), REQ "%s", im->requested);
    return sd_bus_message_append(r, "s", s);
}

static void start_install(struct image *im);

static int p_req_set(sd_bus *b, const char *path, const char *iface, const char *prop, sd_bus_message *v, void *ud,
                     sd_bus_error *e)
{
    (void)b; (void)path; (void)iface; (void)prop;
    struct image *im = ud;
    const char *s;
    int r = sd_bus_message_read(v, "s", &s);
    if (r < 0) return r;
    if (strncmp(s, REQ, strlen(REQ))) return sd_bus_error_set(e, SD_BUS_ERROR_INVALID_ARGS, "bad value");
    s += strlen(REQ);
    if (strcmp(s, "Active") && strcmp(s, "None")) return sd_bus_error_set(e, SD_BUS_ERROR_INVALID_ARGS, "bad value");
    im->requested = !strcmp(s, "Active") ? "Active" : "None";
    emit(im, "xyz.openbmc_project.Software.Activation", "RequestedActivation");
    if (im == &upload && !strcmp(s, "Active") && !strcmp(im->activation, "Ready")) start_install(im);
    return 0;
}

static int p_str(sd_bus *b, const char *path, const char *iface, const char *prop, sd_bus_message *r, void *ud,
                 sd_bus_error *e)
{
    (void)b; (void)path; (void)iface; (void)e;
    struct image *im = ud;
    if (!strcmp(prop, "Version")) return sd_bus_message_append(r, "s", im->version);
    if (!strcmp(prop, "Purpose")) return sd_bus_message_append(r, "s", PURPOSE);
    if (!strcmp(prop, "Path")) return sd_bus_message_append(r, "s", im->path);
    return sd_bus_message_append(r, "s", "");
}

static int p_progress(sd_bus *b, const char *path, const char *iface, const char *prop, sd_bus_message *r, void *ud,
                      sd_bus_error *e)
{
    (void)b; (void)path; (void)iface; (void)prop; (void)e;
    struct image *im = ud;
    return sd_bus_message_append(r, "y", im->progress);
}

static int p_prio(sd_bus *b, const char *path, const char *iface, const char *prop, sd_bus_message *r, void *ud,
                  sd_bus_error *e)
{
    (void)b; (void)path; (void)iface; (void)prop; (void)ud; (void)e;
    return sd_bus_message_append(r, "y", 0);
}

static void remove_upload(void);

static int m_delete(sd_bus_message *m, void *ud, sd_bus_error *e)
{
    struct image *im = ud;
    if (im != &upload || !strcmp(im->activation, "Activating"))
        return sd_bus_error_set(e, "xyz.openbmc_project.Common.Error.NotAllowed", "not allowed");
    remove_upload();
    return sd_bus_reply_method_return(m, "");
}

static const sd_bus_vtable act_vt[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("Activation", "s", p_act, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_WRITABLE_PROPERTY("RequestedActivation", "s", p_act, p_req_set, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_VTABLE_END
};
static const sd_bus_vtable ver_vt[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("Version", "s", p_str, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Purpose", "s", p_str, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_VTABLE_END
};
static const sd_bus_vtable path_vt[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("Path", "s", p_str, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_VTABLE_END
};
static const sd_bus_vtable prog_vt[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("Progress", "y", p_progress, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_VTABLE_END
};
static const sd_bus_vtable prio_vt[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("Priority", "y", p_prio, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_VTABLE_END
};
static const sd_bus_vtable del_vt[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Delete", "", "", m_delete, 0),
    SD_BUS_VTABLE_END
};

/* the associations of /xyz/openbmc_project/software: the running image is
 * functional, active, updateable (bmcweb's FirmwareVersion) */
static int p_assoc(sd_bus *b, const char *path, const char *iface, const char *prop, sd_bus_message *r, void *ud,
                   sd_bus_error *e)
{
    (void)b; (void)path; (void)iface; (void)prop; (void)ud; (void)e;
    int k = sd_bus_message_open_container(r, 'a', "(sss)");
    if (k < 0) return k;
    static const char *const fw[] = { "functional", "active", "updateable" };
    for (int i = 0; i < 3; i++) sd_bus_message_append(r, "(sss)", fw[i], "software_version", running.obj);
    return sd_bus_message_close_container(r);
}
static const sd_bus_vtable assoc_vt[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("Associations", "a(sss)", p_assoc, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_VTABLE_END
};

/* one sd_bus_slot per image would need a vtable list: a fallback-free
 * approach, one slot per interface, all freed with the image */
struct islots
{
    sd_bus_slot *s[6];
};
static struct islots running_slots, upload_slots;

static int add_image(struct image *im, struct islots *sl, int with_delete)
{
    int r = 0;
    snprintf(im->obj, sizeof(im->obj), SW "/%s", im->id);
    memset(sl, 0, sizeof(*sl));
    if (r >= 0) r = sd_bus_add_object_vtable(bus, &sl->s[0], im->obj, "xyz.openbmc_project.Software.Activation", act_vt, im);
    if (r >= 0) r = sd_bus_add_object_vtable(bus, &sl->s[1], im->obj, "xyz.openbmc_project.Software.Version", ver_vt, im);
    if (r >= 0) r = sd_bus_add_object_vtable(bus, &sl->s[2], im->obj, "xyz.openbmc_project.Common.FilePath", path_vt, im);
    if (r >= 0)
        r = sd_bus_add_object_vtable(bus, &sl->s[3], im->obj, "xyz.openbmc_project.Software.ActivationProgress", prog_vt, im);
    if (r >= 0)
        r = sd_bus_add_object_vtable(bus, &sl->s[4], im->obj, "xyz.openbmc_project.Software.RedundancyPriority", prio_vt, im);
    if (r >= 0 && with_delete)
        r = sd_bus_add_object_vtable(bus, &sl->s[5], im->obj, "xyz.openbmc_project.Object.Delete", del_vt, im);
    if (r < 0)
    {
        fprintf(stderr, "reimu-abd: %s: %s\n", im->obj, strerror(-r));
        return r;
    }
    sd_bus_emit_object_added(bus, im->obj);
    return 0;
}

static void drop_image(struct image *im, struct islots *sl)
{
    if (!im->id[0]) return;
    sd_bus_emit_object_removed(bus, im->obj);
    for (int i = 0; i < 6; i++) sl->s[i] = sd_bus_slot_unref(sl->s[i]);
    im->id[0] = 0;
}

static void remove_upload(void)
{
    if (upload.path[0]) unlink(upload.path);
    drop_image(&upload, &upload_slots);
    upload.path[0] = 0;
}

/* ------------------------------------------------------------------ */
/* the update image                                                    */

/* ustar: offset and size of the members; 0 when all three are there */
static int tar_members(int fd, off_t *koff, off_t *ksz, off_t *roff, off_t *rsz, char *why, size_t wsz)
{
    unsigned char h[512];
    off_t off = 0;
    int have = 0;
    *koff = *roff = -1;
    for (int n = 0; n < 64; n++)
    {
        if (pread(fd, h, 512, off) != 512) break;
        if (!h[0]) break;
        if (memcmp(h + 257, "ustar", 5))
        {
            snprintf(why, wsz, "не tar-архив");
            return -1;
        }
        char name[101], szs[13];
        memcpy(name, h, 100);
        name[100] = 0;
        memcpy(szs, h + 124, 12);
        szs[12] = 0;
        off_t sz = strtoll(szs, NULL, 8);
        const char *base = strncmp(name, "./", 2) ? name : name + 2;
        if (!strcmp(base, "image-kernel")) *koff = off + 512, *ksz = sz, have |= 1;
        else if (!strcmp(base, "image-rofs")) *roff = off + 512, *rsz = sz, have |= 2;
        else if (!strcmp(base, "SHA256SUMS")) have |= 4;
        off += 512 + (sz + 511) / 512 * 512;
    }
    if (have != 7)
    {
        snprintf(why, wsz, have ? "в архиве нет image-kernel, image-rofs или SHA256SUMS: это не обновление reimu-ab"
                                : "не tar-архив обновления reimu-ab");
        return -1;
    }
    return 0;
}

static long long mtd_size(const char *label)
{
    char p[96], n[64];
    for (int i = 0; i < 32; i++)
    {
        snprintf(p, sizeof(p), "/sys/class/mtd/mtd%d/name", i);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        int ok = fgets(n, sizeof(n), f) && (n[strcspn(n, "\n")] = 0, !strcmp(n, label));
        fclose(f);
        if (!ok) continue;
        snprintf(p, sizeof(p), "/sys/class/mtd/mtd%d/size", i);
        f = fopen(p, "r");
        long long s = -1;
        if (f && fscanf(f, "%lld", &s) != 1) s = -1;
        if (f) fclose(f);
        return s;
    }
    return -1;
}

/* the version of a reimu-ab update, 0 if it is one */
static int check_update(const char *path, char *version, size_t vsz, char *why, size_t wsz)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
    {
        snprintf(why, wsz, "%s", strerror(errno));
        return -1;
    }
    off_t ko, ks, ro, rs;
    int r = tar_members(fd, &ko, &ks, &ro, &rs, why, wsz);
    long long kpart = mtd_size("kernel-a"), rpart = mtd_size("rofs-a");
    if (!r && kpart > 0 && ks > kpart)
    {
        snprintf(why, wsz, "ядро (%lld байт) больше раздела kernel-a (%lld)", (long long)ks, kpart);
        r = -1;
    }
    if (!r && rpart > 0 && rs != rpart)
    {
        snprintf(why, wsz, "image-rofs (%lld байт) не равен разделу rofs-a (%lld): образ для другой разметки",
                 (long long)rs, rpart);
        r = -1;
    }
    char t[4097] = "";
    if (!r && (rs < 4096 || pread(fd, t, 4096, ro + rs - 4096) != 4096))
    {
        snprintf(why, wsz, "image-rofs обрезан");
        r = -1;
    }
    close(fd);
    if (r) return -1;
    t[4096] = 0;
    char *v = strstr(t, "reimu-version ");
    if (!v || !strstr(t, "reimu-rofs ") || !strstr(t, "reimu-kernel "))
    {
        snprintf(why, wsz, "в image-rofs нет подписи reimu-ab (версии и контрольных сумм)");
        return -1;
    }
    v += 14;
    size_t n = strcspn(v, "\n");
    snprintf(version, vsz, "%.*s", (int)(n < vsz ? n : vsz - 1), v);
    return 0;
}

static void new_file(const char *name)
{
    char path[256], version[128], why[256];
    snprintf(path, sizeof(path), IMAGES "/%s", name);
    struct stat st;
    if (stat(path, &st) || !S_ISREG(st.st_mode)) return;
    last_upload.time = time(NULL);
    if (job.state && !strcmp(job.state, "running"))
    {
        unlink(path);
        snprintf(last_upload.message, sizeof(last_upload.message), "Идёт установка другого образа");
        last_upload.ok = 0;
        log_error("xyz.openbmc_project.Software.Image.Error.BusyFailure", last_upload.message, path);
        return;
    }
    if (check_update(path, version, sizeof(version), why, sizeof(why)))
    {
        unlink(path);
        snprintf(last_upload.message, sizeof(last_upload.message), "Образ отклонён: %s%s", why,
                 strstr(why, "обрезан") ? " (файл не дошёл целиком: не хватило памяти в /tmp?)" : "");
        last_upload.ok = 0;
        fprintf(stderr, "reimu-abd: %s: %s\n", path, why);
        log_error("xyz.openbmc_project.Software.Image.Error.ImageFailure", why, path);
        return;
    }
    /* one upload at a time: a newer one replaces it */
    remove_upload();
    char key[200];
    snprintf(key, sizeof(key), "upload-%s", version);
    make_id(key, upload.id);
    snprintf(upload.path, sizeof(upload.path), "%s", path);
    snprintf(upload.version, sizeof(upload.version), "%s", version);
    upload.activation = "Ready";
    upload.requested = "None";
    upload.progress = 0;
    snprintf(last_upload.message, sizeof(last_upload.message), "Образ %s принят", version);
    last_upload.ok = 1;
    fprintf(stderr, "reimu-abd: %s: update %s, %s\n", path, version, upload.id);
    if (add_image(&upload, &upload_slots, 1) < 0) remove_upload();
}

/* ------------------------------------------------------------------ */
/* the install                                                         */

static sd_event_source *job_io, *job_child;

static void job_line(char *l)
{
    if (strstr(l, ": kernel") && !strstr(l, "written")) set_progress(&upload, 10);
    else if (strstr(l, "kernel written")) set_progress(&upload, 30);
    else if (strstr(l, ": rofs") && !strstr(l, "written")) set_progress(&upload, 35);
    else if (strstr(l, "rofs written")) set_progress(&upload, 90);
    else if (strstr(l, ": intact")) set_progress(&upload, 95);
    if (*l)
    {
        const char *m = strncmp(l, "reimu-ab: ", 10) ? l : l + 10;
        snprintf(job.message, sizeof(job.message), "%s", m);
    }
    fprintf(stderr, "reimu-abd: install: %s\n", l);
}

static int on_job_out(sd_event_source *s, int fd, uint32_t revents, void *ud)
{
    (void)s; (void)revents; (void)ud;
    char tmp[1024];
    ssize_t n = read(fd, tmp, sizeof(tmp));
    if (n <= 0)
    {
        job_io = sd_event_source_unref(job_io);
        close(fd);
        return 0;
    }
    badd(&job.out, tmp, n);
    char *nl;
    while (job.out.s && (nl = memchr(job.out.s, '\n', job.out.len)))
    {
        *nl = 0;
        job_line(job.out.s);
        size_t rest = job.out.len - (nl + 1 - job.out.s);
        memmove(job.out.s, nl + 1, rest);
        job.out.len = rest;
        job.out.s[rest] = 0;
    }
    return 0;
}

static int on_job_exit(sd_event_source *s, const siginfo_t *si, void *ud)
{
    (void)s; (void)ud;
    int ok = si->si_code == CLD_EXITED && si->si_status == 0;
    /* the rest of the output (non-blocking: stops at EOF or when empty) */
    while (job_io) on_job_out(NULL, sd_event_source_get_io_fd(job_io), 0, NULL);
    if (job.out.len)
    {
        job_line(job.out.s);
        job.out.len = 0;
    }
    job.finished = time(NULL);
    job.pid = 0;
    job_child = sd_event_source_unref(job_child);
    if (ok)
    {
        set_progress(&upload, 100);
        job.state = "ok";
        snprintf(job.message, sizeof(job.message), "Образ %s записан в резервный слот и проверен; он запустится после перезагрузки BMC", upload.version);
        set_activation(&upload, "Active");
    }
    else
    {
        job.state = "failed";
        char m[600];
        snprintf(m, sizeof(m), "Установка не удалась: %s", job.message[0] ? job.message : "reimu-ab install");
        snprintf(job.message, sizeof(job.message), "%s", m);
        set_activation(&upload, "Failed");
    }
    fprintf(stderr, "reimu-abd: install %s: %s\n", job.state, job.message);
    /* the image isn't needed any more (RAM) */
    if (upload.path[0]) unlink(upload.path);
    read_slots();
    return 0;
}

static void start_install(struct image *im)
{
    int pfd[2];
    if (job.state && !strcmp(job.state, "running")) return;
    if (pipe2(pfd, O_CLOEXEC)) return;
    fcntl(pfd[0], F_SETFL, O_NONBLOCK);
    memset(job.message, 0, sizeof(job.message));
    snprintf(job.version, sizeof(job.version), "%s", im->version);
    job.started = time(NULL);
    job.finished = 0;
    job.progress = 0;
    job.state = "running";
    im->progress = 0;
    set_activation(im, "Activating");
    pid_t pid = fork();
    if (pid == 0)
    {
        dup2(pfd[1], 1);
        dup2(pfd[1], 2);
        execl(reimu_ab(), "reimu-ab", "install", im->path, (char *)NULL);
        _exit(127);
    }
    close(pfd[1]);
    if (pid < 0)
    {
        close(pfd[0]);
        job.state = "failed";
        snprintf(job.message, sizeof(job.message), "fork: %s", strerror(errno));
        set_activation(im, "Failed");
        return;
    }
    job.pid = pid;
    sd_event_add_io(ev, &job_io, pfd[0], EPOLLIN, on_job_out, NULL);
    sd_event_add_child(ev, &job_child, pid, WEXITED, on_job_exit, NULL);
    snprintf(job.message, sizeof(job.message), "Запись образа %s в резервный слот", im->version);
}

/* ------------------------------------------------------------------ */
/* the web UI object                                                   */

static int p_ab_json(sd_bus *b, const char *path, const char *iface, const char *prop, sd_bus_message *r, void *ud,
                     sd_bus_error *e)
{
    (void)b; (void)path; (void)iface; (void)prop; (void)ud; (void)e;
    if (time(NULL) - slots.read > 60 && !(job.state && !strcmp(job.state, "running"))) read_slots();
    struct buf j = { 0 };
    bprintf(&j, "{\"version\":1,\"now\":%lld,\"running\":", (long long)time(NULL));
    bjson(&j, running.version);
    badd(&j, ",\"booted\":", 10);
    bjson(&j, slots.booted);
    badd(&j, ",\"next\":", 8);
    bjson(&j, slots.next);
    badd(&j, ",\"tries\":", 9);
    bjson(&j, slots.tries);
    bprintf(&j, ",\"pending\":%s,\"slots\":{\"a\":", slots.pending ? "true" : "false");
    bjson(&j, slots.a);
    badd(&j, ",\"b\":", 5);
    bjson(&j, slots.b);
    badd(&j, "},\"job\":", 8);
    if (job.state)
    {
        bprintf(&j, "{\"state\":\"%s\",\"progress\":%d,\"started\":%lld,\"finished\":%lld,\"version\":", job.state,
                job.progress, (long long)job.started, (long long)job.finished);
        bjson(&j, job.version);
        badd(&j, ",\"message\":", 11);
        bjson(&j, job.message);
        badd(&j, "}", 1);
    }
    else badd(&j, "null", 4);
    badd(&j, ",\"upload\":", 10);
    if (last_upload.time)
    {
        bprintf(&j, "{\"ok\":%s,\"time\":%lld,\"message\":", last_upload.ok ? "true" : "false", (long long)last_upload.time);
        bjson(&j, last_upload.message);
        badd(&j, "}", 1);
    }
    else badd(&j, "null", 4);
    badd(&j, "}", 1);
    int k = sd_bus_message_append(r, "s", j.s ? j.s : "{}");
    free(j.s);
    return k;
}

static int m_refresh(sd_bus_message *m, void *ud, sd_bus_error *e)
{
    (void)ud; (void)e;
    read_slots();
    return sd_bus_reply_method_return(m, "s", "{\"ok\":true,\"message\":\"ok\"}");
}

static const sd_bus_vtable ab_vt[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("Json", "s", p_ab_json, 0, SD_BUS_VTABLE_PROPERTY_EMITS_INVALIDATION),
    SD_BUS_METHOD("Refresh", "", "s", m_refresh, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END
};

/* ------------------------------------------------------------------ */

static int on_inotify(sd_event_source *s, int fd, uint32_t revents, void *ud)
{
    (void)s; (void)revents; (void)ud;
    char b[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    ssize_t n = read(fd, b, sizeof(b));
    for (char *p = b; n > 0 && p < b + n;)
    {
        struct inotify_event *e = (struct inotify_event *)p;
        if (e->len && (e->mask & (IN_CLOSE_WRITE | IN_MOVED_TO)) && e->name[0] != '.') new_file(e->name);
        p += sizeof(*e) + e->len;
    }
    return 0;
}

int main(void)
{
    int r = sd_event_default(&ev);
    if (r >= 0) r = sd_bus_default_system(&bus);
    if (r < 0)
    {
        fprintf(stderr, "reimu-abd: %s\n", strerror(-r));
        return 1;
    }
    sd_bus_attach_event(bus, ev, 0);
    /* children are reaped by sd-event */
    sigset_t ss;
    sigemptyset(&ss);
    sigaddset(&ss, SIGCHLD);
    sigprocmask(SIG_BLOCK, &ss, NULL);

    os_version(running.version, sizeof(running.version));
    make_id(running.version, running.id);
    read_slots();

    /* images left by a daemon before (they take RAM): it was not activated */
    mkdir(IMAGES, 0755);
    DIR *d = opendir(IMAGES);
    struct dirent *de;
    while (d && (de = readdir(d)))
        if (de->d_name[0] != '.')
        {
            char p[300];
            snprintf(p, sizeof(p), IMAGES "/%s", de->d_name);
            unlink(p);
        }
    if (d) closedir(d);
    int in = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (in < 0 || inotify_add_watch(in, IMAGES, IN_CLOSE_WRITE | IN_MOVED_TO) < 0)
    {
        fprintf(stderr, "reimu-abd: inotify %s: %s\n", IMAGES, strerror(errno));
        return 1;
    }
    sd_event_add_io(ev, NULL, in, EPOLLIN, on_inotify, NULL);

    r = sd_bus_add_object_manager(bus, NULL, SW);
    if (r >= 0) r = sd_bus_add_object_vtable(bus, NULL, SW, "xyz.openbmc_project.Association.Definitions", assoc_vt, NULL);
    if (r >= 0) r = add_image(&running, &running_slots, 0);
    if (r >= 0) r = sd_bus_add_object_manager(bus, NULL, "/xyz/openbmc_project/reimu/ab");
    if (r >= 0) r = sd_bus_add_object_vtable(bus, NULL, AB_PATH, AB_IFACE, ab_vt, NULL);
    if (r >= 0) r = sd_bus_request_name(bus, BUSNAME, 0);
    if (r < 0)
    {
        fprintf(stderr, "reimu-abd: D-Bus: %s\n", strerror(-r));
        return 1;
    }
    return sd_event_loop(ev) < 0;
}
