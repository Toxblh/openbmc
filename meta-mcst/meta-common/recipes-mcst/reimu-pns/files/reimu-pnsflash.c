/*
 * The PNS flash for the web UI: backups, image upload, check and write,
 * all done by reimu-pns-flash (meta-ab64), whose state files this reads.
 *
 *   object    /xyz/openbmc_project/reimu/pns_flash
 *   interface xyz.openbmc_project.Reimu.PnsFlash
 *     property Json (s)       {"tool", "now", "state" (the job, state.json),
 *                              "chip" (versions last read, chip.json),
 *                              "upload", "backups", "kept"}
 *     method Run(s action, s arg) -> s     JSON {"ok", "message", ...}
 *        check      -            check the uploaded image ("check": the result)
 *        upload-done SHA256      the upload is complete: compare its sha256,
 *                                then check it
 *        discard    -            delete the uploaded image
 *        read       -            back up the chip (a job)
 *        write      NAME         write "upload", a backup (backup-*.bin.gz) or
 *        write-any-board NAME    "last" (the chip before the last write) (a job)
 *        disconnect -            give the flash back to the host
 *     method Upload(t offset, s data) -> s
 *        data (base64) at offset of the upload; offset 0 starts a new one;
 *        "ff:N": N bytes of 0xff (the browser skips erased parts)
 *     method Download(s name, t offset, u length) -> s
 *        a backup (backup-*.bin.gz or "last"), unpacked: offset 0 unpacks it,
 *        the last chunk drops the unpacked copy;
 *        {"ok", "size", "ff" (all 0xff: no data), "data" (base64)}
 *
 * bmcweb's REST API returns only the first output argument of a method,
 * so every result is a single JSON string.
 *
 * Jobs run as the transient unit reimu-pns-job (systemd-run): they outlive
 * this daemon, and only one runs at a time.
 *
 * Copyright (c) 2026 MCST REIMU contributors. Licensed under GPLv2.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <systemd/sd-bus.h>

#include "reimu-sensors.h"
#include "reimu-pnsflash.h"

#define PF_PATH  "/xyz/openbmc_project/reimu/pns_flash"
#define PF_IFACE "xyz.openbmc_project.Reimu.PnsFlash"
#define PF_TOOL  "/usr/sbin/reimu-pns-flash"
#define PF_MAX   (64u << 20) /* the chip */
#define PF_CHUNK (1u << 20)  /* the largest Download */

static const char *env_or(const char *name, const char *def)
{
    const char *v = getenv(name);
    return v && *v ? v : def;
}
#define TOOL  env_or("REIMU_PNS_FLASH", PF_TOOL)
#define RUN   env_or("REIMU_PNS_RUN", "/run/reimu-pns")
#define WORK  env_or("REIMU_PNS_WORK", "/tmp/reimu-pns")
#define KEEP  env_or("REIMU_PNS_KEEP", "/var/lib/reimu-pns")

/* ------------------------------------------------------------------ */

static void reply_json(struct buf *b, int ok, const char *msg)
{
    buf_printf(b, "{\"ok\":%s,\"message\":", ok ? "true" : "false");
    buf_json(b, msg, strlen(msg));
}

static int reply(sd_bus_message *m, int ok, const char *msg, const char *extra)
{
    struct buf b = { 0 };
    reply_json(&b, ok, msg);
    if (extra) buf_str(&b, extra);
    buf_str(&b, "}");
    int r = sd_bus_reply_method_return(m, "s", b.s ? b.s : "{}");
    free(b.s);
    return r;
}

static char *slurp(const char *path, size_t max, size_t *len)
{
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    char *d = malloc(max + 1);
    size_t n = d ? fread(d, 1, max, f) : 0;
    fclose(f);
    if (!d) return NULL;
    while (n && (d[n - 1] == '\n' || d[n - 1] == ' ')) n--;
    d[n] = 0;
    if (len) *len = n;
    return d;
}

/* a JSON object written by reimu-pns-flash, or null */
static void add_file_json(struct buf *b, const char *path)
{
    size_t n;
    char *d = slurp(path, 64 * 1024, &n);
    if (d && n && d[0] == '{' && d[n - 1] == '}') buf_add(b, d, n);
    else buf_str(b, "null");
    free(d);
}

static void path_of(char *out, size_t sz, const char *dir, const char *name)
{
    snprintf(out, sz, "%s/%s", dir, name);
}

/* "upload" (WORK/upload.bin), "backup-YYYYmmdd-HHMMSS.bin.gz" (WORK) or
 * "last" (the chip before the last write, gzipped on the BMC flash) -> path;
 * 0 when the name is valid. reimu-pns-flash writes .gz images as they are. */
static int name_to_path(const char *name, char *out, size_t sz)
{
    if (!strcmp(name, "upload"))
    {
        path_of(out, sz, WORK, "upload.bin");
        return 0;
    }
    if (!strcmp(name, "last"))
    {
        path_of(out, sz, KEEP, "backup-last.bin.gz");
        return 0;
    }
    size_t l = strlen(name);
    if (strncmp(name, "backup-", 7) || l < 15 || l > 64 || strcmp(name + l - 7, ".bin.gz")) return -1;
    for (size_t i = 7; i < l - 7; i++)
        if (!((name[i] >= '0' && name[i] <= '9') || name[i] == '-')) return -1;
    path_of(out, sz, WORK, name);
    return 0;
}

static void add_file_info(struct buf *b, const char *path, const char *name)
{
    struct stat st;
    if (stat(path, &st))
    {
        buf_str(b, "null");
        return;
    }
    buf_str(b, "{\"name\":");
    buf_json(b, name, strlen(name));
    buf_printf(b, ",\"size\":%lld,\"time\":%lld", (long long)st.st_size, (long long)st.st_mtime);
    /* the sha256 of the chip: NAME.bin.sha256 next to NAME.bin.gz */
    char sp[300];
    size_t pl = strlen(path);
    if (pl > 3 && !strcmp(path + pl - 3, ".gz")) pl -= 3;
    snprintf(sp, sizeof(sp), "%.*s.sha256", (int)pl, path);
    size_t n;
    char *s = slurp(sp, 200, &n);
    if (s && n >= 64)
    {
        buf_str(b, ",\"sha256\":");
        buf_json(b, s, 64);
    }
    free(s);
    buf_str(b, "}");
}

static int cmp_desc(const void *a, const void *b)
{
    return strcmp(*(char *const *)b, *(char *const *)a);
}

static char *pf_json(void)
{
    struct buf b = { 0 };
    char p[256];
    buf_printf(&b, "{\"version\":1,\"now\":%lld,\"tool\":%s,\"state\":", (long long)time(NULL),
               access(TOOL, X_OK) == 0 ? "true" : "false");
    path_of(p, sizeof(p), RUN, "state.json");
    add_file_json(&b, p);
    /* a job whose process is gone without its end state */
    size_t n;
    char *st = slurp(p, 64 * 1024, &n);
    if (st && strstr(st, "\"state\":\"running\""))
    {
        char *pp = strstr(st, "\"pid\":");
        int pid = pp ? atoi(pp + 6) : 0;
        if (pid <= 0 || (kill(pid, 0) && errno == ESRCH)) buf_str(&b, ",\"stale\":true");
    }
    free(st);
    buf_str(&b, ",\"chip\":");
    path_of(p, sizeof(p), KEEP, "chip.json");
    add_file_json(&b, p);
    buf_str(&b, ",\"upload\":");
    path_of(p, sizeof(p), WORK, "upload.bin");
    add_file_info(&b, p, "upload");
    buf_str(&b, ",\"kept\":");
    path_of(p, sizeof(p), KEEP, "backup-last.bin.gz");
    add_file_info(&b, p, "last");
    buf_str(&b, ",\"backups\":[");
    DIR *d = opendir(WORK);
    char *names[64];
    int nn = 0;
    if (d)
    {
        struct dirent *e;
        while ((e = readdir(d)) && nn < 64)
        {
            size_t l = strlen(e->d_name);
            if (!strncmp(e->d_name, "backup-", 7) && l > 14 && !strcmp(e->d_name + l - 7, ".bin.gz"))
                names[nn++] = strdup(e->d_name);
        }
        closedir(d);
    }
    qsort(names, nn, sizeof(names[0]), cmp_desc);
    for (int i = 0; i < nn; i++)
    {
        path_of(p, sizeof(p), WORK, names[i]);
        if (i) buf_str(&b, ",");
        add_file_info(&b, p, names[i]);
        free(names[i]);
    }
    buf_str(&b, "]}");
    return b.s;
}

/* ------------------------------------------------------------------ */
/* running the tool                                                    */

/* argv -> exit code, its stdout+stderr in out (at most outsz - 1) */
static int run_tool(char *const argv[], char *out, size_t outsz)
{
    int pfd[2];
    out[0] = 0;
    if (pipe(pfd)) return -1;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0)
    {
        dup2(pfd[1], 1);
        dup2(pfd[1], 2);
        close(pfd[0]);
        close(pfd[1]);
        execv(argv[0], argv);
        _exit(127);
    }
    close(pfd[1]);
    size_t n = 0;
    ssize_t r;
    char tmp[4096];
    while ((r = read(pfd[0], n < outsz - 1 ? out + n : tmp, n < outsz - 1 ? outsz - 1 - n : sizeof(tmp))) > 0)
        if (n < outsz - 1) n += r;
    out[n] = 0;
    close(pfd[0]);
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) return -1;
    for (; n && (out[n - 1] == '\n' || out[n - 1] == ' '); n--) out[n - 1] = 0;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int host_is_off(sd_bus *bus, char *why, size_t sz)
{
    char *st = NULL;
    int r = sd_bus_get_property_string(bus, "xyz.openbmc_project.State.Host", "/xyz/openbmc_project/state/host0",
                                       "xyz.openbmc_project.State.Host", "CurrentHostState", NULL, &st);
    int off = r >= 0 && st && strstr(st, ".Off");
    if (!off)
        snprintf(why, sz, r < 0 ? "Состояние платформы неизвестно: запись и чтение флеша ПНС запрещены"
                                : "Платформа включена: флеш ПНС занят хостом. Выключите платформу и повторите");
    free(st);
    return off;
}

/* a job: reimu-pns-flash ARGS... as the transient unit reimu-pns-job */
static int start_job(sd_bus_message *m, const char *what, char *const args[])
{
    char *argv[20];
    int n = 0;
    argv[n++] = "/usr/bin/systemd-run";
    argv[n++] = "--unit=reimu-pns-job";
    argv[n++] = "--collect";
    argv[n++] = "--description=PNS flash job";
    /* a stop (shutdown) lets the shell finish the write it is in */
    argv[n++] = "-p";
    argv[n++] = "KillMode=mixed";
    /* systemd kills the job's guard with it: whatever ended the job, the
     * flash goes back to the host (and a dead job is marked interrupted) */
    char post[300];
    snprintf(post, sizeof(post), "ExecStopPost=%s disconnect", TOOL);
    argv[n++] = "-p";
    argv[n++] = post;
    argv[n++] = "-p";
    argv[n++] = "TimeoutStopSec=900";
    argv[n++] = (char *)TOOL;
    for (int i = 0; args[i] && n < 19; i++) argv[n++] = args[i];
    argv[n] = NULL;
    char out[1024];
    int rc = run_tool(argv, out, sizeof(out));
    if (rc != 0)
    {
        char msg[1200];
        snprintf(msg, sizeof(msg), strstr(out, "already") ? "Флеш ПНС занят другой операцией"
                                                          : "Не удалось запустить %s: %s", what, out);
        return reply(m, 0, msg, NULL);
    }
    char msg[256];
    snprintf(msg, sizeof(msg), "%s: запущено", what);
    return reply(m, 1, msg, NULL);
}

static int check_upload(sd_bus_message *m, const char *path, const char *okmsg)
{
    char *argv[] = { (char *)TOOL, "check", (char *)path, "--json", NULL };
    static char out[64 * 1024];
    int rc = run_tool(argv, out, sizeof(out));
    size_t n = strlen(out);
    /* the JSON is the last line (the rest: messages) */
    char *j = strrchr(out, '\n');
    j = j ? j + 1 : out;
    struct buf extra = { 0 };
    if (*j == '{' && n && out[n - 1] == '}')
    {
        buf_str(&extra, ",\"check\":");
        buf_str(&extra, j);
    }
    int r = reply(m, rc == 0, rc == 0 ? okmsg : "Образ не прошёл проверку", extra.s);
    free(extra.s);
    return r;
}

static int sha256_file(const char *path, char out[65])
{
    char *argv[] = { "/usr/bin/sha256sum", (char *)path, NULL };
    char o[512];
    if (access(argv[0], X_OK)) argv[0] = "/bin/sha256sum";
    if (run_tool(argv, o, sizeof(o)) != 0 || strlen(o) < 64) return -1;
    memcpy(out, o, 64);
    out[64] = 0;
    return 0;
}

static int method_run(sd_bus_message *m, void *ud, sd_bus_error *err)
{
    (void)ud; (void)err;
    const char *action, *arg;
    int r = sd_bus_message_read(m, "ss", &action, &arg);
    if (r < 0) return r;
    sd_bus *bus = sd_bus_message_get_bus(m);
    char up[256], path[256], why[256];
    path_of(up, sizeof(up), WORK, "upload.bin");
    if (access(TOOL, X_OK)) return reply(m, 0, "Запись флеша ПНС не поддерживается этой прошивкой (нет reimu-pns-flash)", NULL);

    if (!strcmp(action, "check"))
    {
        if (access(up, R_OK)) return reply(m, 0, "Образ не загружен", NULL);
        return check_upload(m, up, "Образ годен для записи");
    }
    if (!strcmp(action, "upload-done"))
    {
        char part[300], sum[65];
        snprintf(part, sizeof(part), "%s.part", up);
        if (access(part, R_OK)) return reply(m, 0, "Загрузка не начата", NULL);
        if (sha256_file(part, sum)) return reply(m, 0, "Не удалось посчитать sha256 загруженного файла", NULL);
        if (strcasecmp(sum, arg))
        {
            unlink(part);
            return reply(m, 0, "Файл повреждён при передаче (sha256 не совпадает): загрузите его снова", NULL);
        }
        if (rename(part, up)) return reply(m, 0, strerror(errno), NULL);
        return check_upload(m, up, "Образ загружен и годен для записи");
    }
    if (!strcmp(action, "discard"))
    {
        char part[300];
        snprintf(part, sizeof(part), "%s.part", up);
        unlink(part);
        unlink(up);
        return reply(m, 1, "Загруженный образ удалён", NULL);
    }
    if (!strcmp(action, "read"))
    {
        if (!host_is_off(bus, why, sizeof(why))) return reply(m, 0, why, NULL);
        char *args[] = { "read", NULL };
        return start_job(m, "Чтение флеша ПНС", args);
    }
    if (!strcmp(action, "write") || !strcmp(action, "write-any-board"))
    {
        if (name_to_path(arg, path, sizeof(path)) || access(path, R_OK))
            return reply(m, 0, "Нет такого образа или резервной копии", NULL);
        if (!host_is_off(bus, why, sizeof(why))) return reply(m, 0, why, NULL);
        char *args[] = { "write", path, !strcmp(action, "write") ? NULL : "--any-board", NULL };
        return start_job(m, "Запись флеша ПНС", args);
    }
    if (!strcmp(action, "disconnect"))
    {
        char *argv[] = { (char *)TOOL, "disconnect", NULL };
        char out[512];
        int rc = run_tool(argv, out, sizeof(out));
        return reply(m, rc == 0, out[0] ? out : "?", NULL);
    }
    return reply(m, 0, "Неизвестное действие", NULL);
}

/* ------------------------------------------------------------------ */
/* base64                                                              */

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void b64_enc(struct buf *b, const unsigned char *d, size_t n)
{
    char q[4];
    for (size_t i = 0; i < n; i += 3)
    {
        unsigned v = d[i] << 16 | (i + 1 < n ? d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        q[0] = B64[v >> 18 & 63];
        q[1] = B64[v >> 12 & 63];
        q[2] = i + 1 < n ? B64[v >> 6 & 63] : '=';
        q[3] = i + 2 < n ? B64[v & 63] : '=';
        buf_add(b, q, 4);
    }
}

/* -> bytes in out (at least 3/4 of strlen(s)), -1 on bad input */
static long b64_dec(const char *s, unsigned char *out)
{
    long n = 0;
    unsigned v = 0;
    int k = 0;
    for (; *s; s++)
    {
        const char *p = strchr(B64, *s);
        if (*s == '=') break;
        if (!p || !*s) return -1;
        v = v << 6 | (unsigned)(p - B64);
        if (++k == 4)
        {
            out[n++] = v >> 16;
            out[n++] = v >> 8;
            out[n++] = v;
            v = 0;
            k = 0;
        }
    }
    if (k == 1) return -1;
    if (k == 2) out[n++] = v >> 4;
    if (k == 3)
    {
        out[n++] = v >> 10;
        out[n++] = v >> 2;
    }
    return n;
}

static int method_upload(sd_bus_message *m, void *ud, sd_bus_error *err)
{
    (void)ud; (void)err;
    uint64_t off;
    const char *data;
    int r = sd_bus_message_read(m, "ts", &off, &data);
    if (r < 0) return r;
    char part[300];
    snprintf(part, sizeof(part), "%s/upload.bin.part", WORK);
    mkdir(WORK, 0755);
    unsigned char *bytes;
    long n;
    if (!strncmp(data, "ff:", 3))
    {
        n = atol(data + 3);
        if (n < 0 || n > (long)PF_MAX) return reply(m, 0, "неверный размер", NULL);
        bytes = malloc(n ? n : 1);
        if (bytes) memset(bytes, 0xff, n);
    }
    else
    {
        bytes = malloc(strlen(data) / 4 * 3 + 4);
        n = bytes ? b64_dec(data, bytes) : -1;
    }
    if (!bytes || n < 0)
    {
        free(bytes);
        return reply(m, 0, "Неверные данные (base64)", NULL);
    }
    if (off + n > PF_MAX)
    {
        free(bytes);
        return reply(m, 0, "Файл больше флеша ПНС (64 МиБ)", NULL);
    }
    int fd = open(part, O_WRONLY | O_CREAT | (off == 0 ? O_TRUNC : 0), 0600);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || (uint64_t)st.st_size != off)
    {
        if (fd >= 0) close(fd);
        free(bytes);
        return reply(m, 0, fd < 0 ? strerror(errno) : "Части файла пришли не по порядку: начните загрузку снова", NULL);
    }
    long done = 0;
    while (done < n)
    {
        ssize_t w = pwrite(fd, bytes + done, n - done, off + done);
        if (w <= 0) break;
        done += w;
    }
    free(bytes);
    if (close(fd) || done != n)
    {
        unlink(part);
        return reply(m, 0, "Не удалось сохранить файл (нет места в /tmp?)", NULL);
    }
    char extra[64];
    snprintf(extra, sizeof(extra), ",\"size\":%llu", (unsigned long long)(off + n));
    return reply(m, 1, "ok", extra);
}

/* a backup (gzipped) unpacked for a download: WORK/.download.bin, NAME in
 * .download.name; unpacked by the first chunk, removed after the last */
static int download_file(const char *name, uint64_t off, char *out, size_t sz)
{
    char gz[256], nf[256];
    if (!strcmp(name, "upload") || name_to_path(name, gz, sizeof(gz)) || access(gz, R_OK)) return -1;
    path_of(out, sz, WORK, ".download.bin");
    path_of(nf, sizeof(nf), WORK, ".download.name");
    if (off == 0)
    {
        char cmd[900];
        unlink(out);
        snprintf(cmd, sizeof(cmd), "gzip -dc '%s' > '%s.tmp' && mv '%s.tmp' '%s' || { rm -f '%s.tmp'; exit 1; }", gz, out,
                 out, out, out);
        if (system(cmd)) return -2;
        FILE *f = fopen(nf, "w");
        if (f)
        {
            fputs(name, f);
            fclose(f);
        }
        return 0;
    }
    size_t n;
    char *cur = slurp(nf, 200, &n);
    int ok = cur && !strcmp(cur, name);
    free(cur);
    return ok ? 0 : -3;
}

static int method_download(sd_bus_message *m, void *ud, sd_bus_error *err)
{
    (void)ud; (void)err;
    const char *name;
    uint64_t off;
    uint32_t len;
    int r = sd_bus_message_read(m, "stu", &name, &off, &len);
    if (r < 0) return r;
    char path[256];
    int k = download_file(name, off, path, sizeof(path));
    if (k == -2) return reply(m, 0, "Не удалось распаковать копию в /tmp (нет места?)", NULL);
    if (k == -3) return reply(m, 0, "Скачивание другой копии начато заново: начните снова", NULL);
    if (k) return reply(m, 0, "Нет такой резервной копии", NULL);
    int fd = open(path, O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st))
    {
        if (fd >= 0) close(fd);
        return reply(m, 0, "Нет такой резервной копии", NULL);
    }
    if (len > PF_CHUNK) len = PF_CHUNK;
    if (off > (uint64_t)st.st_size) off = st.st_size;
    if (off + len > (uint64_t)st.st_size) len = st.st_size - off;
    unsigned char *d = malloc(len ? len : 1);
    ssize_t got = d ? pread(fd, d, len, off) : -1;
    close(fd);
    if (got != (ssize_t)len)
    {
        free(d);
        return reply(m, 0, "Ошибка чтения", NULL);
    }
    /* the last chunk: the unpacked copy isn't needed any more (RAM) */
    if (off + len >= (uint64_t)st.st_size)
    {
        char nf[256];
        unlink(path);
        path_of(nf, sizeof(nf), WORK, ".download.name");
        unlink(nf);
    }
    int ff = 1;
    for (uint32_t i = 0; i < len && ff; i++) ff = d[i] == 0xff;
    struct buf e = { 0 };
    buf_printf(&e, ",\"size\":%lld,\"offset\":%llu,\"length\":%u,\"ff\":%s,\"data\":\"", (long long)st.st_size,
               (unsigned long long)off, len, ff ? "true" : "false");
    if (!ff) b64_enc(&e, d, len);
    buf_str(&e, "\"");
    free(d);
    r = reply(m, 1, "ok", e.s);
    free(e.s);
    return r;
}

static int prop_json(sd_bus *bus, const char *path, const char *iface, const char *prop, sd_bus_message *reply_m,
                     void *ud, sd_bus_error *err)
{
    (void)bus; (void)path; (void)iface; (void)prop; (void)ud; (void)err;
    char *j = pf_json();
    int r = sd_bus_message_append(reply_m, "s", j ? j : "{}");
    free(j);
    return r;
}

static const sd_bus_vtable pf_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("Json", "s", prop_json, 0, SD_BUS_VTABLE_PROPERTY_EMITS_INVALIDATION),
    SD_BUS_METHOD("Run", "ss", "s", method_run, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Upload", "ts", "s", method_upload, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Download", "stu", "s", method_download, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END
};

int pnsflash_register(sd_bus *bus)
{
    return sd_bus_add_object_vtable(bus, NULL, PF_PATH, PF_IFACE, pf_vtable, NULL);
}

