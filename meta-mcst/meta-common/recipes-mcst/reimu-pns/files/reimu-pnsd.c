/*
 * reimu-pnsd: exposes information about the host boot firmware (ПНС) and
 * its SPI flash to bmcweb as D-Bus object
 *   service   xyz.openbmc_project.Reimu.Pns
 *   object    /xyz/openbmc_project/reimu/pns
 *   interface xyz.openbmc_project.Reimu.Pns
 *     property Json (s)  - JSON document, see build_json()
 *     method   Rescan() -> s   JSON {"ok": bool, "message": string}
 *                         re-reads the flash partition table (host must be off)
 *   object    /xyz/openbmc_project/reimu/pns_flash   (reimu-pnsflash.c)
 *     backups, upload, check and write of the flash
 *   object    /xyz/openbmc_project/reimu/sensors
 *   interface xyz.openbmc_project.Reimu.Sensors
 *     property Json (s)  - platform sensors, see sensors_doc() and reimu-sensors.c
 *
 * Data sources:
 *   /var/volatile/motherboard_info.xml   FRU ID, extracted at BMC boot by mcst-fruid
 *                                         (absent: read from the FRU EEPROM itself,
 *                                         REIMU_FRU_EEPROM overrides its sysfs path;
 *                                         the chip answers only while the platform is
 *                                         on, so the last copy is kept in FRU_CACHE)
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

#include "reimu-sensors.h"
#ifndef PARSER_ONLY
#include "reimu-pnsflash.h"
#endif

#define FRU_XML   "/var/volatile/motherboard_info.xml"
#define DTS_FILE  "/var/volatile/motherboard_devtree.dts"
#define SCAN_JSON "/var/lib/reimu-pns/scan.json"
#define FRU_CACHE "/var/lib/reimu-pns/fru.xml"
#define SCAN_TOOL "/usr/libexec/reimu-pns-scan"

#define FPT_SIGNATURE   0xb007e2e2U
#define DTB_SIGNATURE   0xff000003U
#define DTB_MAGIC       0xd00dfeedU
#define DTB_DEFAULTADDR 0x00700000U
#define DTB_DEFAULTSIZE 0x00100000U
#define MAX_TEXT        (256 * 1024)

/* ------------------------------------------------------------------ */
/* small growable string (struct buf: reimu-sensors.h)                 */

void buf_add(struct buf *b, const char *s, size_t n)
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

void buf_str(struct buf *b, const char *s)
{
    buf_add(b, s, strlen(s));
}

void buf_printf(struct buf *b, const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) buf_add(b, tmp, (size_t)n < sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1);
}

/* JSON string literal; invalid UTF-8 bytes are replaced with U+FFFD */
void buf_json(struct buf *b, const char *s, size_t n)
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
/* FRU ID EEPROM (IPMI Platform Management FRU Information Storage)    */
/*                                                                     */
/* mcst-fruid writes FRU_XML once at BMC boot. If the EEPROM did not   */
/* answer then (seen on 1Э8СВ-uATX: no file, service failed with 12),  */
/* the same XML is built here from the EEPROM itself.                  */

#define FRU_MAXAREA 2048

/* a sysfs eeprom file of the FRU ID chip: REIMU_FRU_EEPROM, else the
 * first 24cXX device at address 0x57 (the address the device tree
 * of every REIMU platform uses for the "FRUID" chip) */
static int fru_eeprom_path(char *out, size_t outsz)
{
    const char *env = getenv("REIMU_FRU_EEPROM");
    if (env)
    {
        snprintf(out, outsz, "%s", env);
        return access(out, R_OK);
    }
    for (int bus = 0; bus < 16; bus++)
    {
        char dir[96], name[32] = "";
        snprintf(dir, sizeof(dir), "/sys/bus/i2c/devices/%d-0057", bus);
        snprintf(out, outsz, "%s/name", dir);
        FILE *f = fopen(out, "r");
        if (!f) continue;
        if (!fgets(name, sizeof(name), f)) name[0] = 0;
        fclose(f);
        if (strncmp(name, "24c", 3)) continue;
        snprintf(out, outsz, "%s/eeprom", dir);
        if (access(out, R_OK) == 0) return 0;
    }
    return -1;
}

static void xml_text(struct buf *b, const unsigned char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        switch (s[i])
        {
        case '<': buf_str(b, "&lt;"); break;
        case '>': buf_str(b, "&gt;"); break;
        case '&': buf_str(b, "&amp;"); break;
        default:
            if (s[i] >= 0x20) buf_add(b, (const char *)s + i, 1);
        }
    }
}

/* one type/length-encoded field as text; returns its total size or -1 */
static int fru_field(const unsigned char *p, size_t avail, struct buf *out, int *is_end)
{
    if (!avail) return -1;
    unsigned type = p[0] >> 6, len = p[0] & 0x3f;
    *is_end = p[0] == 0xc1;
    if (*is_end) return 1;
    if (1 + len > avail) return -1;
    const unsigned char *d = p + 1;
    switch (type)
    {
    case 0: /* binary */
        for (unsigned i = 0; i < len; i++) buf_printf(out, i ? " 0x%02X" : "0x%02X", d[i]);
        break;
    case 1: /* BCD plus */
        for (unsigned i = 0; i < len * 2; i++)
        {
            unsigned v = i & 1 ? d[i / 2] & 0xf : d[i / 2] >> 4;
            buf_add(out, &"0123456789 -.???"[v], 1);
        }
        break;
    case 2: /* 6-bit packed ASCII */
        for (unsigned bit = 0; bit + 6 <= len * 8; bit += 6)
        {
            unsigned v = 0;
            for (unsigned k = 0; k < 6; k++) v |= ((d[(bit + k) / 8] >> ((bit + k) % 8)) & 1) << k;
            char c = (char)(v + 0x20);
            buf_add(out, &c, 1);
        }
        break;
    default: /* 8-bit text (UTF-8 in MCST boards) */
        buf_add(out, (const char *)d, len);
    }
    return 1 + len;
}

static void fru_elem(struct buf *x, const char *tag, struct buf *val)
{
    buf_printf(x, "\t\t<%s>", tag);
    if (val->s) xml_text(x, (unsigned char *)val->s, val->len);
    buf_printf(x, "</%s>\n", tag);
}

/* board (with mfg date) or product area: fixed fields, then custom ones */
static void fru_info_area(struct buf *x, const char *area, const unsigned char *a, size_t n, int board)
{
    static const char *const board_f[] = { "manufacturer", "product_name", "serial_number", "part_number",
                                           "fru_file_id" };
    static const char *const prod_f[] = { "manufacturer", "product_name", "part_number", "version",
                                          "serial_number", "asset_tag", "fru_file_id" };
    const char *const *names = board ? board_f : prod_f;
    int nfixed = board ? 5 : 7;
    size_t off = board ? 6 : 3;
    buf_printf(x, "\t<%s>\n", area);
    if (board && n >= 6)
    {
        /* minutes since 1996-01-01 00:00 UTC */
        time_t t = 820454400 + 60 * (time_t)(a[3] | a[4] << 8 | a[5] << 16);
        struct tm tm;
        char s[32];
        gmtime_r(&t, &tm);
        strftime(s, sizeof(s), "%Y-%m-%d %H:%M:%S", &tm);
        buf_printf(x, "\t\t<mfg_date type=\"timestamp\">%lld</mfg_date>\n", (long long)t);
        buf_printf(x, "\t\t<mfg_date type=\"string\">%s</mfg_date>\n", s);
    }
    for (int i = 0; off < n; i++)
    {
        struct buf v = { 0 };
        int end;
        int used = fru_field(a + off, n - off, &v, &end);
        if (used < 0 || end)
        {
            free(v.s);
            break;
        }
        off += used;
        char tag[24];
        if (i < nfixed) snprintf(tag, sizeof(tag), "%s", names[i]);
        else snprintf(tag, sizeof(tag), "extra_field_%d", i - nfixed);
        fru_elem(x, tag, &v);
        free(v.s);
    }
    buf_printf(x, "\t</%s>\n", area);
}

/* MCST internal use area: version byte, then name/value pairs of
 * type/length fields, e.g. "mac_addr" = 6 binary bytes */
static void fru_internal_area(struct buf *x, const unsigned char *a, size_t n)
{
    buf_str(x, "\t<internal_use_area>\n");
    size_t off = 1;
    while (off < n)
    {
        struct buf k = { 0 }, v = { 0 };
        int end, used = fru_field(a + off, n - off, &k, &end);
        if (used < 0 || end || !k.s)
        {
            free(k.s);
            break;
        }
        off += used;
        if (off + 7 <= n && a[off] == 0x06 && strstr(k.s, "mac"))
        {
            const unsigned char *m = a + off + 1;
            buf_printf(&v, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
            used = 7;
        }
        else used = fru_field(a + off, n - off, &v, &end);
        if (used < 0 || end)
        {
            free(k.s);
            free(v.s);
            break;
        }
        off += used;
        int ok = 1;
        for (size_t i = 0; i < k.len; i++)
            if (!(k.s[i] == '_' || (k.s[i] >= 'a' && k.s[i] <= 'z') || (k.s[i] >= '0' && k.s[i] <= '9'))) ok = 0;
        if (ok && k.len) fru_elem(x, k.s, &v);
        free(k.s);
        free(v.s);
    }
    buf_str(x, "\t</internal_use_area>\n");
}

static int fru_area_ok(const unsigned char *a, size_t n)
{
    unsigned sum = 0;
    for (size_t i = 0; i < n; i++) sum += a[i];
    return (sum & 0xff) == 0;
}

/* FRU XML from an EEPROM image (fd), NULL if it isn't a valid FRU */
static char *fru_xml_from_fd(int fd)
{
    unsigned char hdr[8];
    if (pread_full(fd, hdr, 8, 0) || hdr[0] != 0x01 || !fru_area_ok(hdr, 8)) return NULL;
    struct buf x = { 0 };
    buf_str(&x, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<fruinfo>\n");
    for (int ai = 1; ai <= 4; ai++)
    {
        if (ai == 2 || !hdr[ai]) continue; /* chassis area: not used by MCST */
        off_t off = hdr[ai] * 8;
        unsigned char a[FRU_MAXAREA];
        size_t n;
        if (ai == 1)
        {
            /* internal use area has no length: it ends where the next area starts */
            int next = 0;
            for (int k = 2; k <= 5; k++)
                if (hdr[k] > hdr[1] && (!next || hdr[k] < next)) next = hdr[k];
            n = next ? (size_t)(next - hdr[1]) * 8 : 256;
            if (n > sizeof(a) || pread_full(fd, a, n, off)) continue;
            fru_internal_area(&x, a, n);
            continue;
        }
        if (pread_full(fd, a, 2, off)) continue;
        n = a[1] * 8;
        if (n < 8 || n > sizeof(a) || pread_full(fd, a, n, off) || !fru_area_ok(a, n)) continue;
        fru_info_area(&x, ai == 3 ? "board_info_area" : "product_info_area", a, n, ai == 3);
    }
    buf_str(&x, "</fruinfo>\n");
    return x.s;
}

/* REIMU_FRU_CACHE: the web devkit keeps it on tmpfs, off the BMC flash */
static const char *fru_cache(void)
{
    const char *p = getenv("REIMU_FRU_CACHE");
    return p ? p : FRU_CACHE;
}

#ifndef PARSER_ONLY
static sd_bus *g_bus; /* set by the daemon: lets the EEPROM read check host power */
#endif

/* The chip shares the bus with the platform's boot loader (its RTC and FRU
 * code) and is powered only while the platform is on. A read of the
 * unpowered chip blocks for seconds (at24 retries), and touching the bus
 * while the platform powers up is what the BMC must avoid, so the EEPROM is
 * read only after the platform has been running for FRU_SETTLE_S seconds
 * (seen by two property reads at least that far apart), and at most once a
 * minute. */
#define FRU_SETTLE_S 120

/* seconds the platform has been seen running: -1 off, -2 unknown (no
 * D-Bus). Counted from the first call that saw it on */
static long host_running_s(void)
{
#ifndef PARSER_ONLY
    static time_t running_since;
    if (g_bus)
    {
        char *st = NULL;
        int on = sd_bus_get_property_string(g_bus, "xyz.openbmc_project.State.Host", "/xyz/openbmc_project/state/host0",
                                            "xyz.openbmc_project.State.Host", "CurrentHostState", NULL, &st) >= 0 &&
                 st && strstr(st, ".Running");
        free(st);
        time_t now = time(NULL);
        if (!on)
        {
            running_since = 0;
            return -1;
        }
        if (!running_since) running_since = now;
        return now - running_since;
    }
#endif
    return -2;
}

static int fru_worth_trying(void)
{
    static time_t last_try;
    time_t now = time(NULL);
    long up = host_running_s();
    if (up == -1 || (up >= 0 && up < FRU_SETTLE_S)) return 0;
    if (last_try && now - last_try < 60) return 0;
    last_try = now;
    return 1;
}

static char *fru_xml_from_eeprom(void)
{
    static char *cached;
    char path[128];
    if (cached) return strdup(cached);
    if (!fru_worth_trying() || fru_eeprom_path(path, sizeof(path))) return NULL;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    char *x = fru_xml_from_fd(fd);
    close(fd);
    if (!x) return NULL;
    cached = strdup(x);
    /* the chip is powered only while the platform is on: keep a copy for
     * the times the BMC boots or runs with the platform off */
    char tmp[160];
    snprintf(tmp, sizeof(tmp), "%s.tmp", fru_cache());
    if (!getenv("REIMU_FRU_CACHE")) mkdir("/var/lib/reimu-pns", 0755);
    FILE *f = fopen(tmp, "w");
    if (f)
    {
        int ok = fputs(x, f) >= 0;
        if (fclose(f) == 0 && ok) rename(tmp, fru_cache());
        else unlink(tmp);
    }
    return x;
}

/* ------------------------------------------------------------------ */
/* JSON document served over D-Bus                                     */

static char *build_json(void)
{
    struct buf b = { 0 };
    size_t len;
    char *data;

    buf_printf(&b, "{\"version\":1,\"now\":%lld,\"fruXml\":", (long long)time(NULL));
    const char *fru_src = "null";
    data = read_file(FRU_XML, MAX_TEXT, &len);
    if (data) fru_src = "\"xml\"";
    else if ((data = fru_xml_from_eeprom()))
    {
        len = strlen(data);
        fru_src = "\"eeprom\"";
    }
    else if ((data = read_file(fru_cache(), MAX_TEXT, &len))) fru_src = "\"cache\"";
    if (data) buf_json(&b, data, len);
    else buf_str(&b, "null");
    buf_printf(&b, ",\"fruSource\":%s", fru_src);
    free(data);

    buf_str(&b, ",\"devtree\":");
    data = read_file(DTS_FILE, MAX_TEXT, &len);
    if (data) buf_json(&b, data, len);
    else buf_str(&b, "null");
    free(data);

    buf_str(&b, ",\"scan\":");
    /* REIMU_PNS_SCAN_OUT: the web devkit keeps the scan on tmpfs (inherited
     * by the scan tool, which writes the same path) */
    const char *scan = getenv("REIMU_PNS_SCAN_OUT");
    data = read_file(scan ? scan : SCAN_JSON, MAX_TEXT, &len);
    /* scan.json is produced by our own parser; trust but sanity check */
    if (data && len && data[0] == '{') buf_add(&b, data, len);
    else buf_str(&b, "null");
    free(data);
    buf_str(&b, "}");
    return b.s;
}

#ifndef PARSER_ONLY
/* platform sensors (reimu-sensors.c): read only once the platform has run
 * SENS_SETTLE_S seconds (its power-up must not see BMC traffic on the bus),
 * at most every SENS_CACHE_S seconds whatever the number of viewers */
#define SENS_SETTLE_S 30
#define SENS_CACHE_S  3
static char *sensors_doc(int force)
{
    static char *cached;
    static time_t cached_at;
    time_t now = time(NULL);
    long up = force ? -2 : host_running_s();
    const char *platform = up == -1 ? "off" : up >= 0 && up < SENS_SETTLE_S ? "starting" : "on";
    if (!strcmp(platform, "on") && cached && now - cached_at < SENS_CACHE_S && !force) return strdup(cached);
    struct buf b = { 0 };
    buf_printf(&b, "{\"version\":1,\"now\":%lld,\"platform\":\"%s\",", (long long)now, platform);
    if (!strcmp(platform, "on")) sensors_json(&b);
    else buf_str(&b, "\"sensors\":[]");
    buf_str(&b, "}");
    if (!strcmp(platform, "on") && b.s)
    {
        free(cached);
        cached = strdup(b.s);
        cached_at = now;
    }
    return b.s;
}

static int prop_sensors(sd_bus *bus, const char *path, const char *iface, const char *prop, sd_bus_message *reply,
                        void *ud, sd_bus_error *err)
{
    (void)bus; (void)path; (void)iface; (void)prop; (void)ud; (void)err;
    char *j = sensors_doc(0);
    int r = sd_bus_message_append(reply, "s", j ? j : "{}");
    free(j);
    return r;
}

static const sd_bus_vtable sensors_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("Json", "s", prop_sensors, 0, SD_BUS_VTABLE_PROPERTY_EMITS_INVALIDATION),
    SD_BUS_VTABLE_END
};

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

static int method_journal(sd_bus_message *m, void *ud, sd_bus_error *err)
{
    (void)ud; (void)err;
    const char *cur;
    int32_t count;
    int r = sd_bus_message_read(m, "si", &cur, &count);
    if (r < 0) return r;
    struct buf b = { 0 };
    journal_json(&b, cur, count);
    r = sd_bus_reply_method_return(m, "s", b.s ? b.s : "{}");
    free(b.s);
    return r;
}

static const sd_bus_vtable journal_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Read", "si", "s", method_journal, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END
};

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
    g_bus = bus;
    r = sd_bus_add_object_vtable(bus, &slot, "/xyz/openbmc_project/reimu/pns", "xyz.openbmc_project.Reimu.Pns",
                                 pns_vtable, NULL);
    if (r >= 0)
        r = sd_bus_add_object_vtable(bus, NULL, "/xyz/openbmc_project/reimu/sensors", "xyz.openbmc_project.Reimu.Sensors",
                                     sensors_vtable, NULL);
    if (r >= 0)
        r = sd_bus_add_object_vtable(bus, NULL, "/xyz/openbmc_project/reimu/journal", "xyz.openbmc_project.Reimu.Journal",
                                     journal_vtable, NULL);
    /* backups, upload, check and write of the flash (reimu-pnsflash.c) */
    if (r >= 0) r = pnsflash_register(bus);
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
    if (argc == 3 && !strcmp(argv[1], "--fru"))
    {
        int fd = open(argv[2], O_RDONLY);
        char *x = fd >= 0 ? fru_xml_from_fd(fd) : NULL;
        if (fd >= 0) close(fd);
        if (x) fputs(x, stdout);
        else fprintf(stderr, "%s: not a valid FRU image\n", argv[2]);
        int rv = x ? 0 : 1;
        free(x);
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
    if (argc == 2 && !strcmp(argv[1], "--sensors"))
    {
        char *j = sensors_doc(1);
        if (j) puts(j);
        free(j);
        return 0;
    }
    if (argc >= 2 && argc <= 4 && !strcmp(argv[1], "--journal"))
    {
        struct buf b = { 0 };
        journal_json(&b, argc > 3 ? argv[3] : "", argc > 2 ? atoi(argv[2]) : -20);
        if (b.s) puts(b.s);
        free(b.s);
        return 0;
    }
    if (argc == 1) return run_daemon();
#endif
    fprintf(stderr, "Usage: %s                    run D-Bus service\n"
                    "       %s --parse <flash>    print ПНС flash partition table as JSON\n"
                    "       %s --fru <eeprom>     print FRU ID EEPROM image as XML\n"
                    "       %s --json             print the D-Bus JSON document\n"
                    "       %s --sensors          read the platform sensors now (no power check)\n"
                    "       %s --journal [n [cursor]]  print BMC journal entries (n < 0: before cursor)\n",
            argv[0], argv[0], argv[0], argv[0], argv[0], argv[0]);
    return 1;
}
