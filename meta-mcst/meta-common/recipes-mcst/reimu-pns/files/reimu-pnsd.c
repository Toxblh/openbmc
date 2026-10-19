/*
 * reimu-pnsd: exposes information about the host boot firmware (ПНС) and
 * its SPI flash to bmcweb as D-Bus object
 *   service   xyz.openbmc_project.Reimu.Pns
 *   object    /xyz/openbmc_project/reimu/pns
 *   interface xyz.openbmc_project.Reimu.Pns
 *     property Json (s)  - JSON document, see build_json()
 *     method   Rescan() -> s   JSON {"ok": bool, "message": string}
 *                         re-reads the flash partition table (host must be off)
 *
 * Data sources:
 *   /var/volatile/motherboard_info.xml   FRU ID, extracted at BMC boot by mcst-fruid
 *   /var/volatile/motherboard_devtree.dts device tree from the ПНС flash (mcst-fruid)
 *   /var/lib/reimu-pns/scan.json         last flash scan (reimu-pns-scan)
 *
 * REIMU_PNS_SCAN overrides the scan tool path (tmpfs web devkit).
 *
 * Parser mode: reimu-pnsd --parse <mtd device or image file>
 *   prints JSON with flash size and partition table of the ПНС flash.
 *
 * Copyright (c) 2026 MCST REIMU contributors. Licensed under GPLv2.
 */

#define _GNU_SOURCE
#include <endian.h>
#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef PARSER_ONLY
#include <systemd/sd-bus.h>
#endif

#define FRU_XML   "/var/volatile/motherboard_info.xml"
#define DTS_FILE  "/var/volatile/motherboard_devtree.dts"
#define SCAN_JSON "/var/lib/reimu-pns/scan.json"
#define SCAN_TOOL "/usr/libexec/reimu-pns-scan"

#define FPT_SIGNATURE   0xb007e2e2U
#define DTB_SIGNATURE   0xff000003U
#define DTB_MAGIC       0xd00dfeedU
#define DTB_DEFAULTADDR 0x00700000U
#define DTB_DEFAULTSIZE 0x00100000U
#define MAX_TEXT        (256 * 1024)

/* ------------------------------------------------------------------ */
/* small growable string                                               */

struct buf
{
    char *s;
    size_t len, cap;
};

static void buf_add(struct buf *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap)
    {
        size_t cap = b->cap ? b->cap : 4096;
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

static void buf_str(struct buf *b, const char *s)
{
    buf_add(b, s, strlen(s));
}

static void buf_printf(struct buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void buf_printf(struct buf *b, const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) buf_add(b, tmp, (size_t)n < sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1);
}

/* JSON string literal; invalid UTF-8 bytes are replaced with U+FFFD */
static void buf_json(struct buf *b, const char *s, size_t n)
{
    buf_add(b, "\"", 1);
    for (size_t i = 0; i < n; i++)
    {
        unsigned char c = s[i];
        char esc[8];
        if (c == '"' || c == '\\')
        {
            esc[0] = '\\';
            esc[1] = c;
            buf_add(b, esc, 2);
        }
        else if (c == '\n') buf_add(b, "\\n", 2);
        else if (c == '\t') buf_add(b, "\\t", 2);
        else if (c < 0x20 || c == 0x7f)
        {
            snprintf(esc, sizeof(esc), "\\u%04x", c);
            buf_add(b, esc, 6);
        }
        else if (c < 0x80) buf_add(b, (char *)&c, 1);
        else
        {
            /* validate a UTF-8 sequence */
            int len = (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 0;
            int ok = len && i + len <= n;
            for (int k = 1; ok && k < len; k++)
                if ((s[i + k] & 0xc0) != 0x80) ok = 0;
            if (ok)
            {
                buf_add(b, s + i, len);
                i += len - 1;
            }
            else buf_add(b, "\xef\xbf\xbd", 3);
        }
    }
    buf_add(b, "\"", 1);
}

static char *read_file(const char *path, size_t max, size_t *len)
{
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    char *data = malloc(max + 1);
    if (!data)
    {
        fclose(f);
        return NULL;
    }
    *len = fread(data, 1, max, f);
    data[*len] = 0;
    fclose(f);
    return data;
}

/* ------------------------------------------------------------------ */
/* flash parser                                                        */

struct __attribute__((__packed__)) fpt_header
{
    uint32_t signature;
    uint32_t version;
    uint32_t n_entries;
    uint32_t cksum[8];
    uint8_t padding[20];
};

struct __attribute__((__packed__)) fpt_entry
{
    uint32_t signature;
    uint32_t offset;
    uint32_t size;
    uint32_t cksum[8];
    uint8_t padding[20];
};

static const char *part_name(uint32_t sig)
{
    switch (sig)
    {
        case DTB_SIGNATURE: return "device tree (DTB)";
        default: return NULL;
    }
}

static long long flash_size(int fd, const char *path)
{
    struct stat st;
    if (fstat(fd, &st)) return -1;
    if (S_ISREG(st.st_mode)) return st.st_size;
    /* /dev/mtdblockN: size from sysfs */
    const char *p = strstr(path, "mtdblock");
    if (p)
    {
        char sys[128];
        snprintf(sys, sizeof(sys), "/sys/class/mtd/mtd%s/size", p + 8);
        FILE *f = fopen(sys, "r");
        long long sz = -1;
        if (f)
        {
            if (fscanf(f, "%lld", &sz) != 1) sz = -1;
            fclose(f);
        }
        return sz;
    }
    off_t end = lseek(fd, 0, SEEK_END);
    return end;
}

static int pread_full(int fd, void *buf, size_t n, off_t off)
{
    size_t done = 0;
    while (done < n)
    {
        ssize_t r = pread(fd, (char *)buf + done, n - done, off + done);
        if (r <= 0) return -1;
        done += r;
    }
    return 0;
}

static void json_dtb(struct buf *b, int fd, long long size, uint32_t addr, uint32_t maxsize)
{
    uint32_t hdr[2];
    buf_printf(b, "{\"address\":%u,", addr);
    if (addr + 8ULL > (unsigned long long)size || pread_full(fd, hdr, sizeof(hdr), addr))
    {
        buf_str(b, "\"valid\":false,\"error\":\"out of flash\"}");
        return;
    }
    if (be32toh(hdr[0]) != DTB_MAGIC)
    {
        buf_str(b, "\"valid\":false,\"error\":\"no DTB magic\"}");
        return;
    }
    uint32_t real = be32toh(hdr[1]);
    buf_printf(b, "\"valid\":%s,\"size\":%u,\"maxSize\":%u}", real <= maxsize ? "true" : "false", real, maxsize);
}

static int parse_flash(const char *path, struct buf *b)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
    {
        buf_printf(b, "{\"ok\":false,\"error\":\"can't open %s: %s\"}", path, strerror(errno));
        return 1;
    }
    long long size = flash_size(fd, path);
    const long long sector = 4096;
    if (size < 2 * sector)
    {
        buf_printf(b, "{\"ok\":false,\"error\":\"bad flash size %lld\"}", size);
        close(fd);
        return 1;
    }
    long long ptoff = (size > 0x1000000 ? 0x1000000 : size) - sector;
    struct fpt_header h;
    buf_printf(b, "{\"ok\":true,\"device\":");
    buf_json(b, path, strlen(path));
    buf_printf(b, ",\"size\":%lld,\"time\":%lld,\"partitionTable\":", size, (long long)time(NULL));

    uint32_t dtb_addr = DTB_DEFAULTADDR, dtb_size = DTB_DEFAULTSIZE;
    if (pread_full(fd, &h, sizeof(h), ptoff) || le32toh(h.signature) != FPT_SIGNATURE)
    {
        buf_printf(b, "{\"present\":false,\"offset\":%lld}", ptoff);
    }
    else
    {
        uint32_t n = le32toh(h.n_entries);
        uint32_t maxn = (sector - sizeof(h)) / sizeof(struct fpt_entry);
        buf_printf(b, "{\"present\":true,\"offset\":%lld,\"version\":%u,\"entries\":%u,\"checksum\":\"", ptoff,
                   le32toh(h.version), n);
        for (int i = 0; i < 8; i++) buf_printf(b, "%08x", le32toh(h.cksum[i]));
        buf_str(b, "\",\"partitions\":[");
        if (n > maxn) n = maxn;
        for (uint32_t i = 0; i < n; i++)
        {
            struct fpt_entry e;
            if (pread_full(fd, &e, sizeof(e), ptoff + sizeof(h) + i * sizeof(e))) break;
            uint32_t sig = le32toh(e.signature), off = le32toh(e.offset), sz = le32toh(e.size);
            const char *name = part_name(sig);
            buf_printf(b, "%s{\"signature\":\"0x%08x\",\"offset\":%u,\"size\":%u,\"name\":", i ? "," : "", sig, off, sz);
            if (name) buf_json(b, name, strlen(name));
            else buf_str(b, "null");
            buf_printf(b, ",\"inFlash\":%s,\"checksum\":\"", (unsigned long long)off + sz <= (unsigned long long)size ? "true" : "false");
            for (int k = 0; k < 8; k++) buf_printf(b, "%08x", le32toh(e.cksum[k]));
            buf_str(b, "\"}");
            if (sig == DTB_SIGNATURE)
            {
                dtb_addr = off;
                dtb_size = sz;
            }
        }
        buf_str(b, "]}");
    }
    buf_str(b, ",\"dtb\":");
    json_dtb(b, fd, size, dtb_addr, dtb_size);
    buf_str(b, "}");
    close(fd);
    return 0;
}

/* ------------------------------------------------------------------ */
/* JSON document served over D-Bus                                     */

static char *build_json(void)
{
    struct buf b = { 0 };
    size_t len;
    char *data;

    buf_printf(&b, "{\"version\":1,\"now\":%lld,\"fruXml\":", (long long)time(NULL));
    data = read_file(FRU_XML, MAX_TEXT, &len);
    if (data) buf_json(&b, data, len);
    else buf_str(&b, "null");
    free(data);

    buf_str(&b, ",\"devtree\":");
    data = read_file(DTS_FILE, MAX_TEXT, &len);
    if (data) buf_json(&b, data, len);
    else buf_str(&b, "null");
    free(data);

    buf_str(&b, ",\"scan\":");
    data = read_file(SCAN_JSON, MAX_TEXT, &len);
    /* scan.json is produced by our own parser; trust but sanity check */
    if (data && len && data[0] == '{') buf_add(&b, data, len);
    else buf_str(&b, "null");
    free(data);
    buf_str(&b, "}");
    return b.s;
}

#ifndef PARSER_ONLY
static char *last_json;

static int prop_json(sd_bus *bus, const char *path, const char *iface, const char *prop, sd_bus_message *reply,
                     void *ud, sd_bus_error *err)
{
    (void)bus; (void)path; (void)iface; (void)prop; (void)ud; (void)err;
    free(last_json);
    last_json = build_json();
    return sd_bus_message_append(reply, "s", last_json ? last_json : "{}");
}

static int method_rescan(sd_bus_message *m, void *ud, sd_bus_error *err)
{
    (void)ud; (void)err;
    int r;
    char out[512] = "";
    int ok = 0;
    int pipefd[2];
    if (pipe(pipefd) == 0)
    {
        pid_t pid = fork();
        if (pid == 0)
        {
            dup2(pipefd[1], 1);
            dup2(pipefd[1], 2);
            close(pipefd[0]);
            const char *tool = getenv("REIMU_PNS_SCAN");
            if (!tool) tool = SCAN_TOOL;
            execl(tool, tool, (char *)NULL);
            _exit(127);
        }
        close(pipefd[1]);
        size_t n = 0;
        ssize_t r;
        while (n < sizeof(out) - 1 && (r = read(pipefd[0], out + n, sizeof(out) - 1 - n)) > 0) n += r;
        out[n] = 0;
        close(pipefd[0]);
        int status = 0;
        if (pid > 0 && waitpid(pid, &status, 0) == pid) ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }
    for (size_t n = strlen(out); n && (out[n - 1] == '\n' || out[n - 1] == ' '); n--) out[n - 1] = 0;
    sd_bus_emit_properties_changed(sd_bus_message_get_bus(m), "/xyz/openbmc_project/reimu/pns",
                                   "xyz.openbmc_project.Reimu.Pns", "Json", NULL);
    /* bmcweb's REST API returns only the first output argument of a
     * method, so the result is a single JSON string {"ok":..,"message":..} */
    struct buf res = { 0 };
    const char *msg = out[0] ? out : (ok ? "ok" : "scan failed");
    buf_printf(&res, "{\"ok\":%s,\"message\":", ok ? "true" : "false");
    buf_json(&res, msg, strlen(msg));
    buf_str(&res, "}");
    r = sd_bus_reply_method_return(m, "s", res.s ? res.s : "{}");
    free(res.s);
    return r;
}

static const sd_bus_vtable pns_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("Json", "s", prop_json, 0, SD_BUS_VTABLE_PROPERTY_EMITS_INVALIDATION),
    SD_BUS_METHOD("Rescan", "", "s", method_rescan, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END
};

static int run_daemon(void)
{
    sd_bus *bus = NULL;
    sd_bus_slot *slot = NULL;
    int r = sd_bus_default_system(&bus);
    if (r < 0)
    {
        fprintf(stderr, "can't connect to system bus: %s\n", strerror(-r));
        return 1;
    }
    r = sd_bus_add_object_vtable(bus, &slot, "/xyz/openbmc_project/reimu/pns", "xyz.openbmc_project.Reimu.Pns",
                                 pns_vtable, NULL);
    if (r >= 0) r = sd_bus_add_object_manager(bus, NULL, "/xyz/openbmc_project/reimu");
    if (r >= 0) r = sd_bus_request_name(bus, "xyz.openbmc_project.Reimu.Pns", 0);
    if (r < 0)
    {
        fprintf(stderr, "D-Bus setup failed: %s\n", strerror(-r));
        return 1;
    }
    for (;;)
    {
        r = sd_bus_process(bus, NULL);
        if (r < 0) break;
        if (r > 0) continue;
        if (sd_bus_wait(bus, (uint64_t)-1) < 0) break;
    }
    sd_bus_slot_unref(slot);
    sd_bus_unref(bus);
    return r < 0 ? 1 : 0;
}
#endif

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--parse"))
    {
        struct buf b = { 0 };
        int rv = parse_flash(argv[2], &b);
        if (b.s) puts(b.s);
        free(b.s);
        return rv;
    }
    if (argc == 2 && !strcmp(argv[1], "--json"))
    {
        char *j = build_json();
        if (j) puts(j);
        free(j);
        return 0;
    }
#ifndef PARSER_ONLY
    if (argc == 1) return run_daemon();
#endif
    fprintf(stderr, "Usage: %s                    run D-Bus service\n"
                    "       %s --parse <flash>    print ПНС flash partition table as JSON\n"
                    "       %s --json             print the D-Bus JSON document\n",
            argv[0], argv[0], argv[0]);
    return 1;
}
