/*
 * reimu-serial: serial port (COM / SoL) multiplexer with timestamped history.
 *
 * For every configured port the daemon owns the tty exclusively and:
 *   - keeps a ring buffer of the last N lines (default 20000), each line
 *     stamped with the wall clock time (ms) of its first byte;
 *   - persists the history to a text file, so it survives daemon restarts;
 *   - optionally writes a raw log (compatible with old /var/log/sol.log);
 *   - exports a pty (/run/reimu-serial/<name>.pty) for minicom/tmux/screen;
 *   - serves framed (NDJSON) clients on /run/reimu-serial/port<N>.sock,
 *     used by bmcweb websocket /reimu/serial/<N>;
 *   - optionally serves raw clients on abstract socket "\0obmc-console"
 *     (compatibility with bmcweb /console0 and obmc-console-client).
 *
 * Client mode: reimu-serial --dump <name|index> [-n lines] [-f] [-u]
 *
 * Copyright (c) 2026 MCST REIMU contributors. Licensed under GPLv2.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_CONF     "/etc/reimu-serial.conf"
#define RUN_DIR          "/run/reimu-serial"
#define HIST_DIR         "/var/log/reimu-serial"
#define STATE_DIR        "/var/lib/reimu-serial"
#define MAX_PORTS        8
#define MAX_CLIENTS      64
#define MAX_LINE         2048
#define DEFAULT_LINES    20000
#define CLIENT_OBUF_MAX  (8 * 1024 * 1024)
#define CLIENT_IBUF_MAX  (64 * 1024)
#define HISTORY_BATCH    500

enum client_kind { CL_FRAMED, CL_RAW };

struct line
{
    int64_t ts;
    uint32_t len;
    char *buf;
};

struct port
{
    int idx;
    char name[32];
    char title[128];
    char dev[128];
    int baud;
    int maxlines;
    int console_compat;
    char rawlog[256];

    int fd;              /* tty, -1 when it can't be opened */
    int64_t reopen_at;   /* when to retry opening tty */

    struct line *ring;
    int head;            /* next slot to write */
    int count;

    char partial[MAX_LINE];
    int plen;
    int64_t pts;         /* timestamp of the partial line */

    int ptm;             /* pty master */
    int pty_slave;       /* kept open so master never sees EIO */

    int lsock;           /* framed listen socket */
    int csock;           /* raw (obmc-console compatible) listen socket */

    FILE *hist;
    int hist_lines;      /* lines appended since last compaction */
    int rawfd;
};

struct client
{
    int fd;
    struct port *p;
    enum client_kind kind;
    char *obuf;
    size_t olen, ocap;
    char *ibuf;
    size_t ilen;
    int dead;
};

static struct port ports[MAX_PORTS];
static int nports;
static struct client clients[MAX_CLIENTS];
static int nclients;
static volatile sig_atomic_t exit_req;
static int verbose;

static void msg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

#define dbg(...) do { if (verbose) msg(__VA_ARGS__); } while (0)

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void set_nonblock(int fd)
{
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
}

/* ------------------------------------------------------------------ */
/* base64                                                              */

static const char b64chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t b64_encode(const unsigned char *in, size_t len, char *out)
{
    size_t i, o = 0;
    for (i = 0; i + 2 < len; i += 3)
    {
        out[o++] = b64chars[in[i] >> 2];
        out[o++] = b64chars[((in[i] & 3) << 4) | (in[i + 1] >> 4)];
        out[o++] = b64chars[((in[i + 1] & 15) << 2) | (in[i + 2] >> 6)];
        out[o++] = b64chars[in[i + 2] & 63];
    }
    if (i < len)
    {
        out[o++] = b64chars[in[i] >> 2];
        if (i + 1 < len)
        {
            out[o++] = b64chars[((in[i] & 3) << 4) | (in[i + 1] >> 4)];
            out[o++] = b64chars[(in[i + 1] & 15) << 2];
        }
        else
        {
            out[o++] = b64chars[(in[i] & 3) << 4];
            out[o++] = '=';
        }
        out[o++] = '=';
    }
    out[o] = 0;
    return o;
}

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* Decodes in place-safe (out may equal in). Returns decoded length. */
static size_t b64_decode(const char *in, size_t len, unsigned char *out)
{
    size_t o = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < len; i++)
    {
        int v = b64_val(in[i]);
        if (v < 0) continue;
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            out[o++] = (acc >> bits) & 0xff;
        }
    }
    return o;
}

/* ------------------------------------------------------------------ */
/* config                                                              */

static speed_t baud_const(int baud)
{
    switch (baud)
    {
        case 1200: return B1200;
        case 2400: return B2400;
        case 4800: return B4800;
        case 9600: return B9600;
        case 19200: return B19200;
        case 38400: return B38400;
        case 57600: return B57600;
        case 115200: return B115200;
        case 230400: return B230400;
        case 460800: return B460800;
        case 921600: return B921600;
        default: return 0;
    }
}

/*
 * Config format, one port per line:
 *   name device baud [lines=N] [rawlog=PATH] [console] [title...]
 * Example:
 *   host /dev/ttyS0 115200 lines=20000 rawlog=/var/log/sol.log console Консоль платформы (COM1)
 */
static int load_config(const char *path)
{
    FILE *f = fopen(path, "r");
    char buf[1024];
    if (!f)
    {
        msg("can't open config %s: %s", path, strerror(errno));
        return -1;
    }
    while (fgets(buf, sizeof(buf), f) && nports < MAX_PORTS)
    {
        char *s = buf, *tok, *save = NULL;
        struct port *p = &ports[nports];
        int field = 0;

        buf[strcspn(buf, "\r\n")] = 0;
        while (*s == ' ' || *s == '\t') s++;
        if (!*s || *s == '#') continue;

        memset(p, 0, sizeof(*p));
        p->maxlines = DEFAULT_LINES;
        p->fd = p->ptm = p->pty_slave = p->lsock = p->csock = p->rawfd = -1;

        for (tok = strtok_r(s, " \t", &save); tok; tok = strtok_r(NULL, " \t", &save))
        {
            if (field == 0) snprintf(p->name, sizeof(p->name), "%s", tok);
            else if (field == 1) snprintf(p->dev, sizeof(p->dev), "%s", tok);
            else if (field == 2) p->baud = atoi(tok);
            else if (!strncmp(tok, "lines=", 6)) p->maxlines = atoi(tok + 6);
            else if (!strncmp(tok, "rawlog=", 7)) snprintf(p->rawlog, sizeof(p->rawlog), "%s", tok + 7);
            else if (!strcmp(tok, "console")) p->console_compat = 1;
            else
            {
                /* the rest of the line is the title */
                strncat(p->title, tok, sizeof(p->title) - 1);
                if (save && *save)
                {
                    strncat(p->title, " ", sizeof(p->title) - strlen(p->title) - 1);
                    strncat(p->title, save, sizeof(p->title) - strlen(p->title) - 1);
                }
                break;
            }
            field++;
        }
        if (field < 3 || !baud_const(p->baud))
        {
            msg("bad config line for port '%s' (need: name device baud)", p->name);
            continue;
        }
        if (p->maxlines < 100) p->maxlines = 100;
        if (p->maxlines > 1000000) p->maxlines = 1000000;
        if (!p->title[0]) memcpy(p->title, p->name, sizeof(p->name));
        p->idx = nports++;
    }
    fclose(f);
    return nports ? 0 : -1;
}

static void load_baud_override(struct port *p)
{
    char path[256];
    FILE *f;
    int b;
    snprintf(path, sizeof(path), STATE_DIR "/%s.baud", p->name);
    f = fopen(path, "r");
    if (!f) return;
    if (fscanf(f, "%d", &b) == 1 && baud_const(b)) p->baud = b;
    fclose(f);
}

static void save_baud_override(struct port *p)
{
    char path[256];
    FILE *f;
    mkdir(STATE_DIR, 0755);
    snprintf(path, sizeof(path), STATE_DIR "/%s.baud", p->name);
    f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%d\n", p->baud);
    fclose(f);
}

/* ------------------------------------------------------------------ */
/* tty                                                                 */

static int tty_setup(int fd, int baud)
{
    struct termios t;
    if (tcgetattr(fd, &t)) return -1;
    cfmakeraw(&t);
    t.c_cflag |= CLOCAL | CREAD;
    t.c_cflag &= ~(CRTSCTS | CSTOPB | PARENB);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    cfsetispeed(&t, baud_const(baud));
    cfsetospeed(&t, baud_const(baud));
    return tcsetattr(fd, TCSANOW, &t);
}

static void tty_open(struct port *p)
{
    p->fd = open(p->dev, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (p->fd < 0)
    {
        dbg("%s: can't open %s: %s", p->name, p->dev, strerror(errno));
        p->reopen_at = mono_ms() + 5000;
        return;
    }
    if (tty_setup(p->fd, p->baud) && errno != ENOTTY)
        msg("%s: can't configure %s: %s", p->name, p->dev, strerror(errno));
}

static void tty_write(struct port *p, const char *buf, size_t len)
{
    int64_t deadline = mono_ms() + 2000;
    if (p->fd < 0) return;
    while (len)
    {
        ssize_t n = write(p->fd, buf, len);
        if (n > 0)
        {
            buf += n;
            len -= n;
            continue;
        }
        if (n < 0 && errno != EAGAIN && errno != EINTR) return;
        if (mono_ms() > deadline) return;
        struct pollfd pfd = { .fd = p->fd, .events = POLLOUT };
        poll(&pfd, 1, 100);
    }
}

/* ------------------------------------------------------------------ */
/* history                                                             */

/* history file line: "<ts_ms> <escaped text>" */
static void hist_escape(FILE *f, const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++)
    {
        unsigned char c = s[i];
        if (c == '\\') fputs("\\\\", f);
        else if (c < 0x20 || c == 0x7f) fprintf(f, "\\x%02x", c);
        else fputc(c, f);
    }
}

static size_t hist_unescape(char *s)
{
    char *r = s, *w = s;
    while (*r)
    {
        if (r[0] == '\\' && r[1] == '\\') { *w++ = '\\'; r += 2; }
        else if (r[0] == '\\' && r[1] == 'x' && r[2] && r[3])
        {
            char hex[3] = { r[2], r[3], 0 };
            *w++ = (char)strtol(hex, NULL, 16);
            r += 4;
        }
        else *w++ = *r++;
    }
    *w = 0;
    return w - s;
}

static void ring_push(struct port *p, int64_t ts, const char *buf, size_t len)
{
    struct line *l = &p->ring[p->head];
    free(l->buf);
    l->buf = malloc(len ? len : 1);
    if (!l->buf)
    {
        l->len = 0;
        return;
    }
    memcpy(l->buf, buf, len);
    l->len = len;
    l->ts = ts;
    p->head = (p->head + 1) % p->maxlines;
    if (p->count < p->maxlines) p->count++;
}

static struct line *ring_get(struct port *p, int i) /* 0 = oldest */
{
    int start = (p->head - p->count + p->maxlines) % p->maxlines;
    return &p->ring[(start + i) % p->maxlines];
}

static void hist_path(struct port *p, char *buf, size_t sz, const char *suffix)
{
    snprintf(buf, sz, HIST_DIR "/%s.log%s", p->name, suffix);
}

static void hist_load(struct port *p)
{
    char path[256];
    char *buf = NULL;
    size_t cap = 0;
    FILE *f;
    hist_path(p, path, sizeof(path), "");
    f = fopen(path, "r");
    if (!f) return;
    while (getline(&buf, &cap, f) > 0)
    {
        char *sp;
        long long ts;
        buf[strcspn(buf, "\n")] = 0;
        ts = strtoll(buf, &sp, 10);
        if (sp == buf || *sp != ' ') continue;
        size_t len = hist_unescape(sp + 1);
        ring_push(p, ts, sp + 1, len);
    }
    free(buf);
    fclose(f);
}

/* rewrite history file from the ring (keeps it bounded to ~2*maxlines) */
static void hist_compact(struct port *p)
{
    char path[256], tmp[256];
    FILE *f;
    hist_path(p, path, sizeof(path), "");
    hist_path(p, tmp, sizeof(tmp), ".tmp");
    if (p->hist) fclose(p->hist);
    p->hist = NULL;
    f = fopen(tmp, "w");
    if (f)
    {
        for (int i = 0; i < p->count; i++)
        {
            struct line *l = ring_get(p, i);
            fprintf(f, "%lld ", (long long)l->ts);
            hist_escape(f, l->buf, l->len);
            fputc('\n', f);
        }
        fclose(f);
        rename(tmp, path);
    }
    p->hist = fopen(path, "a");
    p->hist_lines = 0;
}

static void hist_append(struct port *p, int64_t ts, const char *buf, size_t len)
{
    if (!p->hist) return;
    fprintf(p->hist, "%lld ", (long long)ts);
    hist_escape(p->hist, buf, len);
    fputc('\n', p->hist);
    if (++p->hist_lines >= p->maxlines) hist_compact(p);
}

static void line_finish(struct port *p)
{
    size_t len = p->plen;
    if (len && p->partial[len - 1] == '\r') len--;
    ring_push(p, p->pts, p->partial, len);
    hist_append(p, p->pts, p->partial, len);
    p->plen = 0;
}

static void lines_feed(struct port *p, int64_t ts, const char *buf, size_t len)
{
    for (size_t i = 0; i < len; i++)
    {
        if (p->plen == 0) p->pts = ts;
        if (buf[i] == '\n')
        {
            line_finish(p);
            continue;
        }
        p->partial[p->plen++] = buf[i];
        if (p->plen == MAX_LINE) line_finish(p);
    }
}

/* ------------------------------------------------------------------ */
/* clients                                                             */

static void client_queue(struct client *c, const char *buf, size_t len)
{
    if (c->dead) return;
    if (c->olen + len > CLIENT_OBUF_MAX)
    {
        dbg("client %d: output overflow, dropping", c->fd);
        c->dead = 1;
        return;
    }
    if (c->olen + len > c->ocap)
    {
        size_t ncap = c->ocap ? c->ocap : 4096;
        while (ncap < c->olen + len) ncap *= 2;
        char *n = realloc(c->obuf, ncap);
        if (!n)
        {
            c->dead = 1;
            return;
        }
        c->obuf = n;
        c->ocap = ncap;
    }
    memcpy(c->obuf + c->olen, buf, len);
    c->olen += len;
}

static void client_flush(struct client *c)
{
    while (c->olen && !c->dead)
    {
        ssize_t n = send(c->fd, c->obuf, c->olen, MSG_NOSIGNAL);
        if (n < 0)
        {
            if (errno == EAGAIN || errno == EINTR) return;
            c->dead = 1;
            return;
        }
        memmove(c->obuf, c->obuf + n, c->olen - n);
        c->olen -= n;
    }
}

static void json_str(struct client *c, const char *s)
{
    char esc[8];
    client_queue(c, "\"", 1);
    for (; *s; s++)
    {
        unsigned char ch = *s;
        if (ch == '"' || ch == '\\')
        {
            esc[0] = '\\';
            esc[1] = ch;
            client_queue(c, esc, 2);
        }
        else if (ch < 0x20)
        {
            snprintf(esc, sizeof(esc), "\\u%04x", ch);
            client_queue(c, esc, 6);
        }
        else client_queue(c, (const char *)&ch, 1);
    }
    client_queue(c, "\"", 1);
}

static void client_printf(struct client *c, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) client_queue(c, buf, n < (int)sizeof(buf) ? (size_t)n : sizeof(buf) - 1);
}

static void client_b64(struct client *c, const char *buf, size_t len)
{
    char *out = malloc(len * 4 / 3 + 8);
    if (!out) return;
    size_t n = b64_encode((const unsigned char *)buf, len, out);
    client_queue(c, "\"", 1);
    client_queue(c, out, n);
    client_queue(c, "\"", 1);
    free(out);
}

static void send_hello(struct client *c)
{
    struct port *p = c->p;
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);

    client_printf(c, "{\"type\":\"hello\",\"version\":1,\"index\":%d,\"name\":", p->idx);
    json_str(c, p->name);
    client_queue(c, ",\"title\":", 9);
    json_str(c, p->title);
    client_queue(c, ",\"device\":", 10);
    json_str(c, p->dev);
    client_printf(c, ",\"baud\":%d,\"maxLines\":%d,\"online\":%s,\"now\":%lld,\"tzOffsetMin\":%ld,\"tz\":",
                  p->baud, p->maxlines, p->fd >= 0 ? "true" : "false",
                  (long long)now_ms(), tm.tm_gmtoff / 60);
    json_str(c, tm.tm_zone ? tm.tm_zone : "");
    client_queue(c, ",\"ports\":[", 10);
    for (int i = 0; i < nports; i++)
    {
        client_printf(c, "%s{\"index\":%d,\"name\":", i ? "," : "", i);
        json_str(c, ports[i].name);
        client_queue(c, ",\"title\":", 9);
        json_str(c, ports[i].title);
        client_queue(c, "}", 1);
    }
    client_queue(c, "]}\n", 3);
}

static void send_history(struct client *c, int maxlines)
{
    struct port *p = c->p;
    int from = p->count > maxlines ? p->count - maxlines : 0;
    int inbatch = 0;
    for (int i = from; i < p->count; i++)
    {
        struct line *l = ring_get(p, i);
        if (!inbatch) client_queue(c, "{\"type\":\"history\",\"lines\":[", 27);
        client_printf(c, "%s[%lld,", inbatch ? "," : "", (long long)l->ts);
        client_b64(c, l->buf, l->len);
        client_queue(c, "]", 1);
        if (++inbatch == HISTORY_BATCH)
        {
            client_queue(c, "]}\n", 3);
            inbatch = 0;
        }
    }
    if (inbatch) client_queue(c, "]}\n", 3);
    client_printf(c, "{\"type\":\"history_end\",\"count\":%d,\"partial\":[%lld,",
                  p->count - from, (long long)(p->plen ? p->pts : 0));
    client_b64(c, p->partial, p->plen);
    client_queue(c, "]}\n", 3);
}

static void broadcast(struct port *p, int64_t ts, const char *buf, size_t len, int except_fd)
{
    for (int i = 0; i < nclients; i++)
    {
        struct client *c = &clients[i];
        if (c->p != p || c->dead || c->fd == except_fd) continue;
        if (c->kind == CL_RAW) client_queue(c, buf, len);
        else
        {
            client_printf(c, "{\"type\":\"data\",\"t\":%lld,\"d\":", (long long)ts);
            client_b64(c, buf, len);
            client_queue(c, "}\n", 2);
        }
    }
}

static void broadcast_status(struct port *p)
{
    for (int i = 0; i < nclients; i++)
    {
        struct client *c = &clients[i];
        if (c->p != p || c->dead || c->kind != CL_FRAMED) continue;
        client_printf(c, "{\"type\":\"status\",\"baud\":%d,\"online\":%s}\n",
                      p->baud, p->fd >= 0 ? "true" : "false");
    }
}

/* extracts "key":"value" (value without escapes) or "key":number */
static int json_get(const char *s, const char *key, char *out, size_t outsz)
{
    char pat[48];
    const char *v;
    size_t n = 0;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    v = strstr(s, pat);
    if (!v) return -1;
    v += strlen(pat);
    while (*v == ' ' || *v == ':') v++;
    if (*v == '"')
    {
        v++;
        while (*v && *v != '"' && n + 1 < outsz) out[n++] = *v++;
    }
    else
    {
        while (*v && *v != ',' && *v != '}' && n + 1 < outsz) out[n++] = *v++;
    }
    out[n] = 0;
    return 0;
}

static void apply_baud(struct port *p, int baud)
{
    if (!baud_const(baud)) return;
    p->baud = baud;
    if (p->fd >= 0) tty_setup(p->fd, baud);
    save_baud_override(p);
    broadcast_status(p);
}

static void framed_command(struct client *c, char *line)
{
    char type[32];
    struct port *p = c->p;
    if (json_get(line, "type", type, sizeof(type))) return;

    if (!strcmp(type, "write"))
    {
        const char *d = strstr(line, "\"d\"");
        if (!d) return;
        d = strchr(d + 3, '"');
        if (!d) return;
        d++;
        const char *e = strchr(d, '"');
        if (!e) return;
        unsigned char *raw = malloc(e - d + 4);
        if (!raw) return;
        size_t n = b64_decode(d, e - d, raw);
        tty_write(p, (char *)raw, n);
        free(raw);
    }
    else if (!strcmp(type, "break"))
    {
        if (p->fd >= 0) tcsendbreak(p->fd, 0);
    }
    else if (!strcmp(type, "baud"))
    {
        char v[16];
        if (!json_get(line, "value", v, sizeof(v))) apply_baud(p, atoi(v));
    }
    else if (!strcmp(type, "history"))
    {
        char v[16];
        int n = p->maxlines;
        if (!json_get(line, "lines", v, sizeof(v)) && atoi(v) > 0) n = atoi(v);
        send_history(c, n);
    }
    else if (!strcmp(type, "clear"))
    {
        for (int i = 0; i < p->maxlines; i++)
        {
            free(p->ring[i].buf);
            p->ring[i].buf = NULL;
            p->ring[i].len = 0;
        }
        p->head = p->count = 0;
        hist_compact(p);
        for (int i = 0; i < nclients; i++)
            if (clients[i].p == p && clients[i].kind == CL_FRAMED)
                client_queue(&clients[i], "{\"type\":\"cleared\"}\n", 19);
    }
    else if (!strcmp(type, "ping"))
    {
        client_printf(c, "{\"type\":\"pong\",\"now\":%lld}\n", (long long)now_ms());
    }
}

static void client_input(struct client *c)
{
    char buf[8192];
    ssize_t n = recv(c->fd, buf, sizeof(buf), 0);
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR))
    {
        c->dead = 1;
        return;
    }
    if (n < 0) return;

    if (c->kind == CL_RAW)
    {
        tty_write(c->p, buf, n);
        return;
    }

    if (c->ilen + n > CLIENT_IBUF_MAX)
    {
        c->dead = 1;
        return;
    }
    memcpy(c->ibuf + c->ilen, buf, n);
    c->ilen += n;

    char *start = c->ibuf, *nl;
    while ((nl = memchr(start, '\n', c->ibuf + c->ilen - start)))
    {
        *nl = 0;
        framed_command(c, start);
        start = nl + 1;
    }
    c->ilen -= start - c->ibuf;
    memmove(c->ibuf, start, c->ilen);
}

static void client_accept(int lsock, struct port *p, enum client_kind kind)
{
    int fd = accept(lsock, NULL, NULL);
    if (fd < 0) return;
    if (nclients == MAX_CLIENTS)
    {
        close(fd);
        return;
    }
    set_nonblock(fd);
    struct client *c = &clients[nclients++];
    memset(c, 0, sizeof(*c));
    c->fd = fd;
    c->p = p;
    c->kind = kind;
    if (kind == CL_FRAMED)
    {
        c->ibuf = malloc(CLIENT_IBUF_MAX);
        if (!c->ibuf) c->dead = 1;
        send_hello(c);
        send_history(c, p->maxlines);
    }
    dbg("%s: client %d connected (%s)", p->name, fd, kind == CL_RAW ? "raw" : "framed");
}

static void clients_reap(void)
{
    for (int i = 0; i < nclients;)
    {
        if (clients[i].dead)
        {
            dbg("client %d disconnected", clients[i].fd);
            close(clients[i].fd);
            free(clients[i].obuf);
            free(clients[i].ibuf);
            clients[i] = clients[--nclients];
        }
        else i++;
    }
}

/* ------------------------------------------------------------------ */
/* sockets and pty                                                     */

static int listen_unix(const char *path, int abstract)
{
    struct sockaddr_un sa;
    socklen_t salen;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    if (abstract)
    {
        size_t l = strlen(path);
        memcpy(sa.sun_path + 1, path, l);
        salen = offsetof(struct sockaddr_un, sun_path) + 1 + l;
    }
    else
    {
        unlink(path);
        snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
        salen = sizeof(sa);
    }
    if (bind(fd, (struct sockaddr *)&sa, salen) || listen(fd, 8))
    {
        msg("can't listen on %s%s: %s", abstract ? "@" : "", path, strerror(errno));
        close(fd);
        return -1;
    }
    if (!abstract) chmod(path, 0660);
    set_nonblock(fd);
    return fd;
}

static void pty_create(struct port *p)
{
    char link[256];
    struct termios t;
    p->ptm = posix_openpt(O_RDWR | O_NOCTTY);
    if (p->ptm < 0 || grantpt(p->ptm) || unlockpt(p->ptm))
    {
        msg("%s: can't create pty", p->name);
        if (p->ptm >= 0) close(p->ptm);
        p->ptm = -1;
        return;
    }
    set_nonblock(p->ptm);
    p->pty_slave = open(ptsname(p->ptm), O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (p->pty_slave >= 0 && !tcgetattr(p->pty_slave, &t))
    {
        cfmakeraw(&t);
        tcsetattr(p->pty_slave, TCSANOW, &t);
    }
    snprintf(link, sizeof(link), RUN_DIR "/%s.pty", p->name);
    unlink(link);
    if (symlink(ptsname(p->ptm), link))
        msg("%s: can't link %s: %s", p->name, link, strerror(errno));
}

/* ------------------------------------------------------------------ */
/* main loop                                                           */

static void port_input(struct port *p)
{
    char buf[4096];
    ssize_t n = read(p->fd, buf, sizeof(buf));
    if (n <= 0)
    {
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;
        msg("%s: tty read failed (%s), reopening", p->name, n ? strerror(errno) : "EOF");
        close(p->fd);
        p->fd = -1;
        p->reopen_at = mono_ms() + 2000;
        broadcast_status(p);
        return;
    }
    int64_t ts = now_ms();
    lines_feed(p, ts, buf, n);
    if (p->rawfd >= 0 && write(p->rawfd, buf, n) < 0)
    {
        /* raw log may be rotated away (copytruncate keeps fd valid) */
    }
    if (p->ptm >= 0 && write(p->ptm, buf, n) < 0)
    {
        /* nobody reads the pty; drop data, it's already in history */
        if (p->pty_slave >= 0) tcflush(p->pty_slave, TCIFLUSH);
    }
    broadcast(p, ts, buf, n, -1);
}

static void pty_input(struct port *p)
{
    char buf[4096];
    ssize_t n = read(p->ptm, buf, sizeof(buf));
    if (n > 0) tty_write(p, buf, n);
}

static void on_signal(int sig)
{
    (void)sig;
    exit_req = 1;
}

static int run_daemon(const char *conf)
{
    char path[256];

    if (load_config(conf)) return 1;
    mkdir(RUN_DIR, 0755);
    mkdir(HIST_DIR, 0755);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    /* history files and sockets are shared with the web group only */
    umask(007);

    for (int i = 0; i < nports; i++)
    {
        struct port *p = &ports[i];
        load_baud_override(p);
        p->ring = calloc(p->maxlines, sizeof(struct line));
        if (!p->ring)
        {
            msg("%s: out of memory", p->name);
            return 1;
        }
        hist_load(p);
        hist_compact(p);
        if (p->rawlog[0])
            p->rawfd = open(p->rawlog, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0640);
        tty_open(p);
        pty_create(p);
        snprintf(path, sizeof(path), RUN_DIR "/port%d.sock", i);
        p->lsock = listen_unix(path, 0);
        if (p->console_compat) p->csock = listen_unix("obmc-console", 1);
        msg("%s: %s @ %d, %d lines of history, %d loaded", p->name, p->dev, p->baud,
            p->maxlines, p->count);
    }

    while (!exit_req)
    {
        struct pollfd pfd[MAX_PORTS * 4 + MAX_CLIENTS];
        void *owner[MAX_PORTS * 4 + MAX_CLIENTS];
        int kind[MAX_PORTS * 4 + MAX_CLIENTS];
        int n = 0, timeout = 1000;
        int64_t mnow = mono_ms();

        for (int i = 0; i < nports; i++)
        {
            struct port *p = &ports[i];
            if (p->fd < 0 && mnow >= p->reopen_at)
            {
                tty_open(p);
                if (p->fd >= 0) broadcast_status(p);
            }
#define ADD(f, k, o) do { pfd[n].fd = (f); pfd[n].events = POLLIN; kind[n] = (k); owner[n] = (o); n++; } while (0)
            if (p->fd >= 0) ADD(p->fd, 0, p);
            if (p->ptm >= 0) ADD(p->ptm, 1, p);
            if (p->lsock >= 0) ADD(p->lsock, 2, p);
            if (p->csock >= 0) ADD(p->csock, 3, p);
        }
        for (int i = 0; i < nclients; i++)
        {
            ADD(clients[i].fd, 4, &clients[i]);
            if (clients[i].olen) pfd[n - 1].events |= POLLOUT;
        }
#undef ADD

        if (poll(pfd, n, timeout) < 0)
        {
            if (errno == EINTR) continue;
            break;
        }

        for (int i = 0; i < n; i++)
        {
            if (!pfd[i].revents) continue;
            struct port *p = owner[i];
            struct client *c = owner[i];
            switch (kind[i])
            {
                case 0: port_input(p); break;
                case 1: pty_input(p); break;
                case 2: client_accept(p->lsock, p, CL_FRAMED); break;
                case 3: client_accept(p->csock, p, CL_RAW); break;
                case 4:
                    if (pfd[i].revents & (POLLIN | POLLHUP | POLLERR)) client_input(c);
                    break;
            }
        }
        for (int i = 0; i < nclients; i++) client_flush(&clients[i]);
        clients_reap();
        for (int i = 0; i < nports; i++)
            if (ports[i].hist) fflush(ports[i].hist);
    }

    for (int i = 0; i < nports; i++)
    {
        if (ports[i].plen) line_finish(&ports[i]);
        if (ports[i].hist) fclose(ports[i].hist);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* client (dump) mode                                                  */

static void print_line(int64_t ts, const char *buf, size_t len, int utc)
{
    time_t t = ts / 1000;
    struct tm tm;
    char tbuf[64];
    if (utc) gmtime_r(&t, &tm);
    else localtime_r(&t, &tm);
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &tm);
    printf("[%s.%03d] ", tbuf, (int)(ts % 1000));
    fwrite(buf, 1, len, stdout);
    putchar('\n');
}

static int run_dump(const char *conf, const char *which, int lines, int follow, int utc)
{
    char *buf = NULL;
    size_t cap = 0, len = 0;
    int idx = -1, fd;
    struct sockaddr_un sa;

    if (which[0] >= '0' && which[0] <= '9') idx = atoi(which);
    else if (!load_config(conf))
    {
        for (int i = 0; i < nports; i++)
            if (!strcmp(ports[i].name, which)) idx = i;
    }
    if (idx < 0)
    {
        msg("unknown port '%s'", which);
        return 1;
    }

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), RUN_DIR "/port%d.sock", idx);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)))
    {
        msg("can't connect to %s: %s", sa.sun_path, strerror(errno));
        return 1;
    }

    /* partial line for follow mode */
    char cur[MAX_LINE];
    size_t curlen = 0;
    int64_t curts = 0;
    int history_done = 0;
    /* history can't be limited server-side for the initial dump, so we keep
     * a window of the last `lines` lines and print it at history_end */
    struct line *win = lines > 0 ? calloc(lines, sizeof(struct line)) : NULL;
    int whead = 0, wcount = 0;

    for (;;)
    {
        if (len + 65536 > cap)
        {
            cap = cap ? cap * 2 : 131072;
            buf = realloc(buf, cap);
        }
        ssize_t n = read(fd, buf + len, cap - len - 1);
        if (n <= 0) break;
        len += n;
        buf[len] = 0;

        char *start = buf, *nl;
        while ((nl = memchr(start, '\n', buf + len - start)))
        {
            char type[32];
            *nl = 0;
            if (!json_get(start, "type", type, sizeof(type)))
            {
                if (!strcmp(type, "history"))
                {
                    char *s = strstr(start, "\"lines\":[");
                    s = s ? s + 9 : NULL;
                    while (s && (s = strchr(s, '[')))
                    {
                        long long ts = strtoll(s + 1, &s, 10);
                        char *q1 = strchr(s, '"'), *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
                        if (!q2) break;
                        size_t dl = b64_decode(q1 + 1, q2 - q1 - 1, (unsigned char *)q1 + 1);
                        if (win)
                        {
                            struct line *l = &win[whead];
                            free(l->buf);
                            l->buf = malloc(dl + 1);
                            memcpy(l->buf, q1 + 1, dl);
                            l->len = dl;
                            l->ts = ts;
                            whead = (whead + 1) % lines;
                            if (wcount < lines) wcount++;
                        }
                        else print_line(ts, q1 + 1, dl, utc);
                        s = q2 + 1;
                    }
                }
                else if (!strcmp(type, "history_end") && !history_done)
                {
                    history_done = 1;
                    for (int i = 0; win && i < wcount; i++)
                    {
                        struct line *l = &win[(whead - wcount + i + lines) % lines];
                        print_line(l->ts, l->buf, l->len, utc);
                    }
                    fflush(stdout);
                    if (!follow) return 0;
                    char *q = strstr(start, "\"partial\":[");
                    if (q)
                    {
                        char *e;
                        curts = strtoll(q + 11, &e, 10);
                        char *q1 = strchr(e, '"');
                        char *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
                        if (q2) curlen = b64_decode(q1 + 1, q2 - q1 - 1, (unsigned char *)cur);
                    }
                }
                else if (!strcmp(type, "data") && history_done)
                {
                    char t[32];
                    json_get(start, "t", t, sizeof(t));
                    char *q1 = strstr(start, "\"d\":\"");
                    char *q2 = q1 ? strchr(q1 + 5, '"') : NULL;
                    if (q2)
                    {
                        size_t dl = b64_decode(q1 + 5, q2 - q1 - 5, (unsigned char *)q1 + 5);
                        for (size_t i = 0; i < dl; i++)
                        {
                            char ch = q1[5 + i];
                            if (!curlen) curts = atoll(t);
                            if (ch == '\n' || curlen == MAX_LINE - 1)
                            {
                                if (curlen && cur[curlen - 1] == '\r') curlen--;
                                print_line(curts, cur, curlen, utc);
                                curlen = 0;
                            }
                            else cur[curlen++] = ch;
                        }
                        fflush(stdout);
                    }
                }
            }
            start = nl + 1;
        }
        len -= start - buf;
        memmove(buf, start, len);
    }
    return 0;
}

static void usage(void)
{
    fprintf(stderr,
            "Usage: reimu-serial [-c CONF] [-v]                 run daemon\n"
            "       reimu-serial --dump PORT [-n N] [-f] [-u]   print history with timestamps\n"
            "   PORT is port name or index; -f follows new lines; -u prints UTC times\n");
}

int main(int argc, char **argv)
{
    const char *conf = DEFAULT_CONF, *dump = NULL;
    int lines = 0, follow = 0, utc = 0, opt;
    static struct option lopts[] = {
        { "config", 1, 0, 'c' }, { "dump", 1, 0, 'd' }, { "lines", 1, 0, 'n' },
        { "follow", 0, 0, 'f' }, { "utc", 0, 0, 'u' }, { "verbose", 0, 0, 'v' },
        { "help", 0, 0, 'h' }, { 0, 0, 0, 0 } };

    while ((opt = getopt_long(argc, argv, "c:d:n:fuvh", lopts, NULL)) != -1)
    {
        switch (opt)
        {
            case 'c': conf = optarg; break;
            case 'd': dump = optarg; break;
            case 'n': lines = atoi(optarg); break;
            case 'f': follow = 1; break;
            case 'u': utc = 1; break;
            case 'v': verbose = 1; break;
            default: usage(); return opt == 'h' ? 0 : 1;
        }
    }
    if (dump) return run_dump(conf, dump, lines, follow, utc);
    return run_daemon(conf);
}
