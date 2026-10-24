/* reimu-hostlink: the protocol with the host agent and what the BMC keeps of
 * it. JSON lines {"v":1,"t":TYPE,...} both ways (README.md of
 * meta-mcst/tools/host has the whole of it):
 *
 *   BMC -> agent  hello {nonce}            at once, on every connection
 *   agent -> BMC  auth {id, cnonce, mac}   mac = HMAC-SHA256(key,
 *                   "reimu-hostlink-auth-v1\n" nonce "\n" cnonce "\n" id)
 *              or pair {token, id, host}   the one-time token of Pair()
 *   BMC -> agent  welcome {mac, policy}    mac = HMAC-SHA256(key,
 *                   "reimu-hostlink-srv-v1\n" cnonce "\n" nonce)
 *                 paired {key}             then closed
 *                 error {code}             auth, pair, version, malformed,
 *                                          rate, ...: then closed
 *   agent -> BMC  inventory {data}, metrics {data}, event {data},
 *                 result {id, ok, text|err}, pong
 *   BMC -> agent  policy {gpu_fan {mode, duty}, turbo_left}, ping,
 *                 control {id, op, ...}, inventory?
 *
 * Limits: HL_PRE_LINE bytes a line before auth, HL_MAX_LINE after; a token
 * bucket of HL_RATE_BURST messages refilled HL_RATE_PER_S a second;
 * AUTH_TIMEOUT for the hello; unknown types and fields are ignored, another
 * major version ("v") is refused.
 *
 * Copyright (c) 2026 MCST REIMU contributors. Licensed under GPLv2.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/sha.h>
#include <openssl/x509.h>
#include <openssl/hmac.h>

#include "hostlink.h"

int hl_quiet;
void (*hl_log_sink)(int prio, const char *line);

const struct hl_sensor_def hl_sensors[NSENS] = {
    [S_GPU_EDGE] = { "Host_GPU_Edge", "temperature", "DegreesC", "gpu.temp_edge", -40, 150, 0 },
    [S_GPU_JUNCTION] = { "Host_GPU_Junction", "temperature", "DegreesC", "gpu.temp_junction", -40, 150, 1 },
    [S_GPU_MEM] = { "Host_GPU_Memory", "temperature", "DegreesC", "gpu.temp_mem", -40, 150, 2 },
    [S_GPU_FAN] = { "Host_GPU_Fan", "fan_tach", "RPMS", "gpu.fan_rpm", 0, 6000, -1 },
    [S_GPU_PWM] = { "Host_GPU_Fan_PWM", "fan_pwm", "Percent", "gpu.pwm_pct", 0, 100, -1 },
    [S_GPU_POWER] = { "Host_GPU_Power", "power", "Watts", "gpu.power_w", 0, 400, -1 },
    [S_DISK_MAX] = { "Host_Disk_Max", "temperature", "DegreesC", NULL, -40, 150, -1 },
};
static const char *const temp_name[3] = { "edge", "junction", "memory" };

/* ------------------------------------------------------------------ util */
void hl_log(int prio, const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (hl_log_sink) hl_log_sink(prio, line);
    if (!hl_quiet) fprintf(stderr, "<%d>%s\n", prio, line);
}

int hl_random(void *p, size_t n)
{
    unsigned char *b = p;
    while (n)
    {
        ssize_t r = getrandom(b, n, 0);
        if (r < 0)
        {
            if (errno == EINTR) continue;
            return -1;
        }
        b += r;
        n -= r;
    }
    return 0;
}

void hl_hex(const unsigned char *p, size_t n, char *out)
{
    static const char x[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++)
    {
        out[2 * i] = x[p[i] >> 4];
        out[2 * i + 1] = x[p[i] & 15];
    }
    out[2 * n] = 0;
}

static int unhex(const char *s, unsigned char *out, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        unsigned v;
        if (sscanf(s + 2 * i, "%2x", &v) != 1) return -1;
        out[i] = v;
    }
    return 0;
}

static int is_hex(const char *s, size_t min, size_t max)
{
    size_t n = strlen(s);
    if (n < min || n > max) return 0;
    for (; *s; s++)
        if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f'))) return 0;
    return 1;
}

void hl_hmac_hex(const unsigned char *key, size_t klen, const char *msg, char *out)
{
    unsigned char md[32];
    unsigned int mlen = 0;
    HMAC(EVP_sha256(), key, (int)klen, (const unsigned char *)msg, strlen(msg), md, &mlen);
    hl_hex(md, 32, out);
}

static int64_t wall(void)
{
    return (int64_t)time(NULL);
}

static void path_of(const struct hl *h, const char *name, char *p, size_t n)
{
    snprintf(p, n, "%s/%s", h->dir, name);
}

/* writes a small file atomically; 0 ok */
static int put_file(const struct hl *h, const char *name, const char *text, mode_t mode)
{
    if (!h->dir[0]) return 0;
    char p[256], tmp[264];
    path_of(h, name, p, sizeof(p));
    snprintf(tmp, sizeof(tmp), "%s.new", p);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0) return -1;
    size_t l = strlen(text);
    int ok = write(fd, text, l) == (ssize_t)l && fsync(fd) == 0;
    close(fd);
    if (!ok || chmod(tmp, mode) || rename(tmp, p))
    {
        unlink(tmp);
        return -1;
    }
    return 0;
}

static int get_file(const struct hl *h, const char *name, char *buf, size_t n)
{
    if (!h->dir[0]) return -1;
    char p[256];
    path_of(h, name, p, sizeof(p));
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t r = read(fd, buf, n - 1);
    close(fd);
    if (r <= 0) return -1;
    buf[r] = 0;
    return 0;
}

/* ------------------------------------------------------------------ config */
void hl_config_defaults(struct hl_config *c)
{
    memset(c, 0, sizeof(*c));
    c->port = HL_PORT;
    snprintf(c->iface, sizeof(c->iface), "eth0");
    c->allow_ipv4 = 1;
    c->ping_ms = 10000;
    c->lost_ms = 60000;
    c->auth_timeout_ms = 10000;
    c->control_timeout_ms = 30000;
    c->pair_ttl_ms = 15 * 60000;
    c->warn[0] = 85, c->crit[0] = 95;
    c->warn[1] = 95, c->crit[1] = 105;
    c->warn[2] = 95, c->crit[2] = 105;
    c->follow_fand_turbo = 1;
    c->logging = 1;
    snprintf(c->https_cert, sizeof(c->https_cert), "/etc/ssl/certs/https/server.pem");
    snprintf(c->disk_helper, sizeof(c->disk_helper), "/usr/libexec/reimu-agent-disk");
}

int hl_config_parse(struct hl_config *c, const char *text, char err[][128], int maxerr)
{
    static const struct { const char *k; int kind; size_t off; double lo, hi; } keys[] = {
#define O(f) offsetof(struct hl_config, f)
        { "PORT", 'i', O(port), 1, 65535 },
        { "IFACE", 's', O(iface), 0, 0 },
        { "ALLOW_IPV4", 'b', O(allow_ipv4), 0, 0 },
        { "PING_SEC", 'm', O(ping_ms), 2, 300 },
        { "LOST_SEC", 'm', O(lost_ms), 10, 3600 },
        { "AUTH_TIMEOUT_SEC", 'm', O(auth_timeout_ms), 2, 120 },
        { "CONTROL_TIMEOUT_SEC", 'm', O(control_timeout_ms), 5, 600 },
        { "PAIR_TTL_SEC", 'm', O(pair_ttl_ms), 60, 86400 },
        { "GPU_EDGE_WARN", 'd', O(warn[0]), 30, 150 },
        { "GPU_EDGE_CRIT", 'd', O(crit[0]), 30, 150 },
        { "GPU_JUNCTION_WARN", 'd', O(warn[1]), 30, 150 },
        { "GPU_JUNCTION_CRIT", 'd', O(crit[1]), 30, 150 },
        { "GPU_MEM_WARN", 'd', O(warn[2]), 30, 150 },
        { "GPU_MEM_CRIT", 'd', O(crit[2]), 30, 150 },
        { "FOLLOW_FAND_TURBO", 'b', O(follow_fand_turbo), 0, 0 },
        { "LOGGING", 'b', O(logging), 0, 0 },
        { "HTTPS_CERT", 'p', O(https_cert), 0, 0 },
        { "DISK_HELPER", 'p', O(disk_helper), 0, 0 },
#undef O
    };
    int nerr = 0;
    const char *p = text;
    while (p && *p)
    {
        const char *e = strchr(p, '\n');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        char line[256];
        if (l >= sizeof(line)) l = sizeof(line) - 1;
        memcpy(line, p, l);
        line[l] = 0;
        p = e ? e + 1 : NULL;
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (!*s || *s == '#') continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = s, *v = eq + 1;
        for (char *t = eq - 1; t >= k && (*t == ' ' || *t == '\t'); t--) *t = 0;
        while (*v == ' ' || *v == '\t') v++;
        size_t vl = strlen(v);
        while (vl && (v[vl - 1] == ' ' || v[vl - 1] == '\t' || v[vl - 1] == '\r')) v[--vl] = 0;
        if (vl >= 2 && (v[0] == '"' || v[0] == '\'') && v[vl - 1] == v[0]) v[vl - 1] = 0, v++;
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
        {
            if (strcmp(k, keys[i].k)) continue;
            char *ptr = (char *)c + keys[i].off, *end;
            int bad = 0;
            if (keys[i].kind == 's') snprintf(ptr, 16, "%s", v);
            else if (keys[i].kind == 'p') snprintf(ptr, 128, "%s", v);
            else if (keys[i].kind == 'b')
            {
                if (!strcmp(v, "yes") || !strcmp(v, "1") || !strcmp(v, "true")) *(int *)ptr = 1;
                else if (!strcmp(v, "no") || !strcmp(v, "0") || !strcmp(v, "false")) *(int *)ptr = 0;
                else bad = 1;
            }
            else
            {
                double d = strtod(v, &end);
                if (end == v || *end || d < keys[i].lo || d > keys[i].hi) bad = 1;
                else if (keys[i].kind == 'i') *(int *)ptr = (int)d;
                else if (keys[i].kind == 'm') *(int *)ptr = (int)(d * 1000);
                else *(double *)ptr = d;
            }
            if (bad)
            {
                if (nerr < maxerr) snprintf(err[nerr], 128, "%s=%.60s: bad value, default kept", k, v);
                nerr++;
            }
        }
    }
    return nerr;
}

/* ------------------------------------------------------------------ state */
static void load_policy(struct hl *h)
{
    char b[64], m[16];
    double d;
    snprintf(h->fan_mode, sizeof(h->fan_mode), "auto");
    h->fan_duty = 0;
    if (get_file(h, "policy", b, sizeof(b)) == 0 && sscanf(b, "%15s %lf", m, &d) == 2)
    {
        if (!strcmp(m, "manual") && d >= 20 && d <= 100)
        {
            snprintf(h->fan_mode, sizeof(h->fan_mode), "manual");
            h->fan_duty = d;
        }
    }
}

static void save_policy(struct hl *h)
{
    char b[64];
    snprintf(b, sizeof(b), "%s %g\n", h->fan_mode, h->fan_duty);
    if (put_file(h, "policy", b, 0644)) hl_log(4, "GPU fan policy not saved: %s", strerror(errno));
}

void hl_init(struct hl *h, const struct hl_config *c, const char *dir)
{
    struct hl_config cc = *c;
    char d[sizeof(h->dir)];
    snprintf(d, sizeof(d), "%s", dir ? dir : "");
    char fp[65], mac[18], v4[16];
    memcpy(fp, h->fingerprint, sizeof(fp));
    memcpy(mac, h->mac, sizeof(mac));
    memcpy(v4, h->ipv4, sizeof(v4));
    buf_free(&h->inventory); /* h is zeroed or initialized before */
    buf_free(&h->metrics);
    memset(h, 0, sizeof(*h));
    h->c = cc;
    memcpy(h->dir, d, sizeof(h->dir));
    /* what the net layer learnt survives a re-init */
    memcpy(h->fingerprint, fp, sizeof(fp));
    memcpy(h->mac, mac, sizeof(mac));
    memcpy(h->ipv4, v4, sizeof(v4));
    h->fingerprint[64] = h->mac[17] = h->ipv4[15] = 0;
    for (int i = 0; i < NSENS; i++) h->sens[i] = NAN;
    h->next_id = 1;
    char b[160];
    if (get_file(h, "key", b, sizeof(b)) == 0 && strlen(b) >= 64)
    {
        b[64] = 0;
        if (is_hex(b, 64, 64) && unhex(b, h->key, HL_KEY_LEN) == 0) h->paired = 1;
        OPENSSL_cleanse(b, sizeof(b));
    }
    load_policy(h);
    long long ce, ue;
    if (get_file(h, "edac", b, sizeof(b)) == 0 && sscanf(b, "%lld %lld", &ce, &ue) == 2) h->ce = ce, h->ue = ue;
}

void hl_event(struct hl *h, int prio, const char *kind, const char *fmt, ...)
{
    struct hl_event *e = &h->ev[h->evpos];
    h->evpos = (h->evpos + 1) % HL_MAX_EVENTS;
    if (h->nev < HL_MAX_EVENTS) h->nev++;
    memset(e, 0, sizeof(*e));
    e->wall = wall();
    e->prio = prio;
    snprintf(e->kind, sizeof(e->kind), "%s", kind);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->text, sizeof(e->text), fmt, ap);
    va_end(ap);
    hl_log(prio, "%s: %s", kind, e->text);
    if (h->on_event) h->on_event(h, e);
    h->gen++;
}

int hl_connected(const struct hl *h)
{
    return h->agent != NULL;
}

const char *hl_heartbeat(const struct hl *h)
{
    if (!h->ever) return "never";
    if (h->agent) return h->now - h->last_seen <= 30000 ? "ok" : "late";
    return h->lost_reported ? "lost" : "late";
}

double hl_gpu_temp(const struct hl *h)
{
    return !isnan(h->sens[S_GPU_JUNCTION]) ? h->sens[S_GPU_JUNCTION] : h->sens[S_GPU_EDGE];
}

/* ------------------------------------------------------------------ sending */
static void send_raw(struct hl_conn *c, const char *line)
{
    if (c->close_why[0] && c->state == C_CLOSING) return;
    if (c->out.len > (1 << 20))
    {
        snprintf(c->close_why, sizeof(c->close_why), "the peer doesn't read");
        return;
    }
    buf_add(&c->out, line, strlen(line));
    buf_add(&c->out, "\n", 1);
}

static void send_error(struct hl_conn *c, const char *code, const char *text)
{
    struct buf b = { 0 };
    buf_printf(&b, "{\"v\":%d,\"t\":\"error\",\"code\":", HL_PROTO);
    buf_json_str(&b, code);
    buf_printf(&b, ",\"text\":");
    buf_json_str(&b, text);
    buf_printf(&b, "}");
    send_raw(c, b.s ? b.s : "");
    buf_free(&b);
    snprintf(c->close_why, sizeof(c->close_why), "%s: %s", code, text);
    c->state = C_CLOSING;
}

static int turbo_left(const struct hl *h)
{
    return h->turbo_until > h->now ? (int)((h->turbo_until - h->now + 999) / 1000) : 0;
}

static void policy_json(const struct hl *h, struct buf *b)
{
    buf_printf(b, "{\"gpu_fan\":{\"mode\":\"%s\",\"duty\":", h->fan_mode);
    if (!strcmp(h->fan_mode, "manual")) buf_printf(b, "%g", h->fan_duty);
    else buf_printf(b, "null");
    buf_printf(b, "},\"turbo_left\":%d}", turbo_left(h));
}

static void push_policy(struct hl *h)
{
    h->gen++;
    if (!h->agent) return;
    struct buf b = { 0 }, p = { 0 };
    policy_json(h, &p);
    /* {"v":1,"t":"policy",<the fields of p>} */
    buf_printf(&b, "{\"v\":%d,\"t\":\"policy\",%s", HL_PROTO, p.s ? p.s + 1 : "}");
    send_raw(h->agent, b.s ? b.s : "");
    buf_free(&b);
    buf_free(&p);
}

/* ------------------------------------------------------------------ connections */
static int s_conn_ids;

void hl_conn_open(struct hl *h, struct hl_conn *c, const char *peer, int64_t now)
{
    memset(c, 0, sizeof(*c));
    h->now = now;
    c->id = ++s_conn_ids;
    c->state = C_HELLO;
    snprintf(c->peer, sizeof(c->peer), "%s", peer ? peer : "?");
    if (!strncmp(c->peer, "fe80:", 5)) c->transport = T_IPV6_LL;
    else if (!strncmp(c->peer, "::ffff:", 7) || !strchr(c->peer, ':')) c->transport = T_IPV4;
    else c->transport = T_IPV6;
    c->opened = c->last_rx = c->tokens_at = now;
    c->tokens = HL_RATE_BURST;
    if (c->transport == T_IPV4 && !h->c.allow_ipv4)
    {
        snprintf(c->close_why, sizeof(c->close_why), "IPv4 not allowed (ALLOW_IPV4=no)");
        c->state = C_CLOSING;
        return;
    }
    unsigned char r[16];
    if (hl_random(r, sizeof(r)))
    {
        snprintf(c->close_why, sizeof(c->close_why), "no random numbers");
        c->state = C_CLOSING;
        return;
    }
    hl_hex(r, sizeof(r), c->nonce);
    char l[200];
    snprintf(l, sizeof(l), "{\"v\":%d,\"t\":\"hello\",\"server\":\"reimu-hostlink\",\"ver\":\"%s\",\"nonce\":\"%s\"}", HL_PROTO,
             HL_VERSION, c->nonce);
    send_raw(c, l);
}

void hl_conn_free(struct hl_conn *c)
{
    buf_free(&c->in);
    buf_free(&c->out);
}

void hl_conn_closed(struct hl *h, struct hl_conn *c, int64_t now)
{
    h->now = now;
    if (h->agent != c) return;
    h->agent = NULL;
    h->gone_at = now;
    hl_event(h, 6, "agent_disconnected", "host agent disconnected (%s)", c->close_why[0] ? c->close_why : "closed");
}

void hl_conn_tick(struct hl *h, struct hl_conn *c, int64_t now)
{
    if (c->state == C_HELLO && now - c->opened > h->c.auth_timeout_ms && !c->close_why[0])
    {
        snprintf(c->close_why, sizeof(c->close_why), "auth timeout");
        c->state = C_CLOSING;
    }
}

/* a string field into out; 0 ok */
static int sfield(const char *s, const struct jtok *t, const char *path, char *out, size_t n)
{
    out[0] = 0;
    return json_str(s, t, json_get(s, t, 0, path), out, n);
}

static void printable(char *s)
{
    for (; *s; s++)
        if ((unsigned char)*s < 0x20 || *s == 0x7f) *s = '?';
}

static void auth_failed(struct hl *h, struct hl_conn *c, const char *why)
{
    h->auth_fails++;
    /* at most a line a minute: a peer guessing must not fill the journal */
    if (!h->auth_fail_logged || h->now - h->auth_fail_logged >= 60000)
    {
        hl_log(4, "agent from %s refused: %s (%d refused so far)", c->peer, why, h->auth_fails);
        h->auth_fail_logged = h->now;
    }
    send_error(c, "auth", why);
}

static void do_auth(struct hl *h, struct hl_conn *c, const char *s, const struct jtok *t)
{
    char id[65], cn[65], mac[65], ver[17], host[65];
    if (!h->paired) return auth_failed(h, c, "not paired");
    if (sfield(s, t, "id", id, sizeof(id)) || !id[0] || sfield(s, t, "cnonce", cn, sizeof(cn)) || !is_hex(cn, 32, 64) ||
        sfield(s, t, "mac", mac, sizeof(mac)) || !is_hex(mac, 64, 64))
        return auth_failed(h, c, "bad auth message");
    printable(id);
    char msg[300], want[65];
    snprintf(msg, sizeof(msg), "reimu-hostlink-auth-v1\n%s\n%s\n%s", c->nonce, cn, id);
    hl_hmac_hex(h->key, HL_KEY_LEN, msg, want);
    if (CRYPTO_memcmp(want, mac, 64)) return auth_failed(h, c, "wrong key");
    c->state = C_AUTHED; /* its nonce is used up: no second auth on it */
    h->authed_at = h->now;
    if (h->agent && h->agent != c)
    {
        snprintf(h->agent->close_why, sizeof(h->agent->close_why), "replaced by a new connection");
        h->agent->state = C_CLOSING;
    }
    h->agent = c;
    snprintf(h->agent_id, sizeof(h->agent_id), "%s", id);
    sfield(s, t, "ver", ver, sizeof(ver));
    sfield(s, t, "host", host, sizeof(host));
    printable(ver);
    printable(host);
    snprintf(h->agent_ver, sizeof(h->agent_ver), "%s", ver);
    snprintf(h->agent_host, sizeof(h->agent_host), "%s", host);
    snprintf(h->agent_peer, sizeof(h->agent_peer), "%s", c->peer);
    h->agent_transport = c->transport;
    h->agent_since_wall = h->last_seen_wall = wall();
    h->last_seen = h->now;
    h->last_ping = h->now;
    static const char *const tn[] = { "IPv4", "IPv6 link-local", "IPv6" };
    if (h->lost_reported) hl_event(h, 5, "agent_back", "host agent %s back (%s, %s)", host, tn[c->transport], c->peer);
    else hl_event(h, h->ever ? 6 : 5, "agent_connected", "host agent %s %s connected (%s, %s)", host, ver, tn[c->transport],
                  c->peer);
    h->ever = 1;
    h->lost_reported = 0;
    h->gone_at = 0;
}

static void welcome(struct hl *h, struct hl_conn *c, const char *cn, const char *nonce)
{
    char msg[300], mac[65];
    snprintf(msg, sizeof(msg), "reimu-hostlink-srv-v1\n%s\n%s", cn, nonce);
    hl_hmac_hex(h->key, HL_KEY_LEN, msg, mac);
    struct buf b = { 0 }, p = { 0 };
    policy_json(h, &p);
    buf_printf(&b, "{\"v\":%d,\"t\":\"welcome\",\"server\":\"reimu-hostlink\",\"ver\":\"%s\",\"mac\":\"%s\",\"ping\":%d,\"policy\":%s}",
               HL_PROTO, HL_VERSION, mac, h->c.ping_ms / 1000, p.s ? p.s : "{}");
    send_raw(c, b.s ? b.s : "");
    buf_free(&b);
    buf_free(&p);
}

static void do_pair(struct hl *h, struct hl_conn *c, const char *s, const struct jtok *t)
{
    char tok[65], id[65], host[65];
    if (!h->token[0] || h->now > h->token_until)
    {
        h->token[0] = 0;
        hl_log(4, "pairing attempt from %s, no pairing active", c->peer);
        return send_error(c, "pair", "no pairing active (Pair on the BMC first)");
    }
    if (sfield(s, t, "token", tok, sizeof(tok)) || strlen(tok) != 32 || CRYPTO_memcmp(tok, h->token, 32))
    {
        if (++h->token_tries >= 5)
        {
            OPENSSL_cleanse(h->token, sizeof(h->token));
            hl_event(h, 4, "pairing_void", "5 wrong pairing tokens: pairing cancelled");
        }
        else hl_log(4, "pairing attempt from %s: wrong token", c->peer);
        OPENSSL_cleanse(tok, sizeof(tok));
        return send_error(c, "pair", "wrong token");
    }
    unsigned char key[HL_KEY_LEN];
    char hex[2 * HL_KEY_LEN + 2];
    if (hl_random(key, sizeof(key))) return send_error(c, "pair", "no random numbers");
    hl_hex(key, sizeof(key), hex);
    strcat(hex, "\n");
    int saved = put_file(h, "key", hex, 0600);
    hex[64] = 0;
    if (saved)
    {
        OPENSSL_cleanse(hex, sizeof(hex));
        return send_error(c, "pair", "the key could not be saved");
    }
    if (h->agent && h->agent != c)
    {
        snprintf(h->agent->close_why, sizeof(h->agent->close_why), "paired again");
        h->agent->state = C_CLOSING;
    }
    memcpy(h->key, key, sizeof(key));
    OPENSSL_cleanse(key, sizeof(key));
    h->paired = 1;
    h->paired_at = h->now;
    OPENSSL_cleanse(h->token, sizeof(h->token));
    OPENSSL_cleanse(h->pair_str, sizeof(h->pair_str));
    sfield(s, t, "id", id, sizeof(id));
    sfield(s, t, "host", host, sizeof(host));
    printable(host);
    char l[200];
    snprintf(l, sizeof(l), "{\"v\":%d,\"t\":\"paired\",\"key\":\"%s\"}", HL_PROTO, hex);
    send_raw(c, l);
    OPENSSL_cleanse(hex, sizeof(hex));
    OPENSSL_cleanse(l, sizeof(l));
    hl_event(h, 5, "paired", "host agent %s paired (from %s)", host, c->peer);
    snprintf(c->close_why, sizeof(c->close_why), "paired");
    c->state = C_CLOSING;
}

/* ------------------------------------------------------------------ the agent's data */
static void edac_check(struct hl *h, const char *s, const struct jtok *t, int data)
{
    int e = json_get(s, t, data, "edac");
    int loaded = 0;
    if (e < 0 || json_bool(t, json_get(s, t, e, "loaded"), &loaded) || !loaded) return;
    double ce = 0, ue = 0;
    if (json_num(s, t, json_get(s, t, e, "ce_total"), &ce) || json_num(s, t, json_get(s, t, e, "ue_total"), &ue)) return;
    /* the DIMMs whose counts went up */
    struct { char label[32]; long long ce, ue; } d[HL_MAX_DIMMS];
    int nd = 0;
    int mcs = json_get(s, t, e, "mcs");
    for (int i = 0; mcs >= 0 && json_item(t, mcs, i) >= 0; i++)
    {
        int ds = json_get(s, t, json_item(t, mcs, i), "dimms");
        for (int k = 0; ds >= 0 && json_item(t, ds, k) >= 0 && nd < HL_MAX_DIMMS; k++)
        {
            int di = json_item(t, ds, k);
            double a = 0, b = 0;
            json_num(s, t, json_get(s, t, di, "ce"), &a);
            json_num(s, t, json_get(s, t, di, "ue"), &b);
            if (json_str(s, t, json_get(s, t, di, "label"), d[nd].label, sizeof(d[nd].label)) || !d[nd].label[0])
                snprintf(d[nd].label, sizeof(d[nd].label), "mc%d dimm%d", i, k);
            printable(d[nd].label);
            d[nd].ce = (long long)a;
            d[nd].ue = (long long)b;
            nd++;
        }
    }
    char which[2][120] = { "", "" };
    for (int k = 0; k < nd; k++)
    {
        long long pce = 0, pue = 0;
        for (int j = 0; j < h->ndimm; j++)
            if (!strcmp(h->dimm[j].label, d[k].label)) pce = h->dimm[j].ce, pue = h->dimm[j].ue;
        if (d[k].ce > pce)
            snprintf(which[0] + strlen(which[0]), sizeof(which[0]) - strlen(which[0]), "%s%s +%lld", which[0][0] ? ", " : "",
                     d[k].label, d[k].ce - pce);
        if (d[k].ue > pue)
            snprintf(which[1] + strlen(which[1]), sizeof(which[1]) - strlen(which[1]), "%s%s +%lld", which[1][0] ? ", " : "",
                     d[k].label, d[k].ue - pue);
    }
    memcpy(h->dimm, d, sizeof(d[0]) * nd);
    h->ndimm = nd;
    long long nce = (long long)ce, nue = (long long)ue;
    if (nue > h->ue)
        hl_event(h, 2, "ecc_ue", "host memory: UNCORRECTABLE ECC errors %lld -> %lld%s%s%s", h->ue, nue, which[1][0] ? " (" : "",
                 which[1], which[1][0] ? ")" : "");
    if (nce > h->ce)
        hl_event(h, 4, "ecc_ce", "host memory: corrected ECC errors %lld -> %lld%s%s%s", h->ce, nce, which[0][0] ? " (" : "",
                 which[0], which[0][0] ? ")" : "");
    if (nce != h->ce || nue != h->ue || !h->have_edac)
    {
        /* lower: the host rebooted (or reloaded EDAC), counting from there */
        h->ce = nce, h->ue = nue;
        char b[64];
        snprintf(b, sizeof(b), "%lld %lld\n", nce, nue);
        put_file(h, "edac", b, 0644);
        h->gen++;
    }
    h->have_edac = 1;
}

static void temp_alarms(struct hl *h)
{
    static const int idx[3] = { S_GPU_EDGE, S_GPU_JUNCTION, S_GPU_MEM };
    for (int i = 0; i < 3; i++)
    {
        double v = h->sens[idx[i]];
        if (isnan(v)) continue;
        int lvl = v >= h->c.crit[i] ? 2 : v >= h->c.warn[i] ? 1 : v < h->c.warn[i] - 5 ? 0 : h->alarm[i];
        if (lvl > h->alarm[i])
            hl_event(h, lvl == 2 ? 2 : 4, "gpu_hot", "GPU %s %.0f °C (%s %.0f °C)", temp_name[i], v,
                     lvl == 2 ? "critical" : "warning", lvl == 2 ? h->c.crit[i] : h->c.warn[i]);
        else if (lvl == 0 && h->alarm[i])
            hl_event(h, 5, "gpu_temp_ok", "GPU %s %.0f °C: back below %.0f °C", temp_name[i], v, h->c.warn[i] - 5);
        h->alarm[i] = lvl;
    }
}

static void store(struct buf *b, const char *s, const struct jtok *t, int i)
{
    b->len = 0;
    buf_add(b, s + t[i].start, t[i].end - t[i].start);
}

static void on_metrics(struct hl *h, const char *s, const struct jtok *t, int data)
{
    store(&h->metrics, s, t, data);
    h->metrics_at = h->now;
    double old[NSENS];
    memcpy(old, h->sens, sizeof(old));
    for (int i = 0; i < NSENS; i++)
    {
        double v = NAN;
        if (hl_sensors[i].path)
        {
            if (json_num(s, t, json_get(s, t, data, hl_sensors[i].path), &v)) v = NAN;
        }
        else
        {
            int ds = json_get(s, t, data, "disks");
            for (int k = 0; ds >= 0 && json_item(t, ds, k) >= 0; k++)
            {
                double d;
                if (!json_num(s, t, json_get(s, t, json_item(t, ds, k), "temp"), &d) && (isnan(v) || d > v)) v = d;
            }
        }
        if (!isnan(v) && (v < hl_sensors[i].min || v > hl_sensors[i].max * 2)) v = NAN;
        h->sens[i] = v;
    }
    if (memcmp(old, h->sens, sizeof(old))) h->sens_gen++;
    temp_alarms(h);
    edac_check(h, s, t, data);
    h->gen++;
}

static void on_result(struct hl *h, const char *s, const struct jtok *t)
{
    double id;
    if (json_num(s, t, json_get(s, t, 0, "id"), &id)) return;
    for (int i = 0; i < h->nres; i++)
    {
        struct hl_result *r = &h->res[i];
        if (r->id != (int)id || r->done) continue;
        int ok = 0;
        json_bool(t, json_get(s, t, 0, "ok"), &ok);
        char txt[160] = "";
        if (sfield(s, t, ok ? "text" : "err", txt, sizeof(txt))) sfield(s, t, "text", txt, sizeof(txt));
        printable(txt);
        r->done = 1;
        r->ok = ok;
        snprintf(r->text, sizeof(r->text), "%s", txt);
        hl_event(h, ok ? 6 : 4, "control", "host: %s %s%s%s", r->op, ok ? "done" : "failed", txt[0] ? ": " : "", txt);
        return;
    }
}

static void on_host_event(struct hl *h, const char *s, const struct jtok *t, int data)
{
    char kind[24] = "", text[140] = "";
    json_str(s, t, json_get(s, t, data, "kind"), kind, sizeof(kind));
    json_str(s, t, json_get(s, t, data, "text"), text, sizeof(text));
    printable(kind);
    printable(text);
    hl_event(h, 5, "host", "%s: %s", kind[0] ? kind : "event", text);
}

/* ------------------------------------------------------------------ input */
static void line(struct hl *h, struct hl_conn *c, const char *s, size_t n)
{
    if (n && s[n - 1] == '\r') n--;
    if (!n) return;
    /* rate: every line takes a token */
    c->tokens += (h->now - c->tokens_at) * HL_RATE_PER_S / 1000.0;
    if (c->tokens > HL_RATE_BURST) c->tokens = HL_RATE_BURST;
    c->tokens_at = h->now;
    if (c->tokens < 1) return send_error(c, "rate", "rate limit");
    c->tokens -= 1;

    int maxt = (int)(n / 2 + 8);
    if (maxt > 40000) maxt = 40000;
    struct jtok *t = malloc(sizeof(*t) * maxt);
    if (!t) return send_error(c, "busy", "out of memory");
    int nt = json_parse(s, n, t, maxt);
    double v = 0;
    char type[24] = "";
    if (nt <= 0 || t[0].type != J_OBJ || (nt > 0 && json_str(s, t, json_get(s, t, 0, "t"), type, sizeof(type))))
    {
        free(t);
        if (c->state == C_HELLO) return send_error(c, "malformed", "not a JSON object with \"t\"");
        if (++c->malformed > 10) snprintf(c->close_why, sizeof(c->close_why), "malformed lines"), c->state = C_CLOSING;
        return;
    }
    if (json_num(s, t, json_get(s, t, 0, "v"), &v) || v != HL_PROTO)
    {
        free(t);
        return send_error(c, "version", "protocol version 1 only");
    }
    if (c->state == C_HELLO)
    {
        if (!strcmp(type, "auth"))
        {
            char cn[65] = "", nonce[33];
            memcpy(nonce, c->nonce, sizeof(nonce));
            sfield(s, t, "cnonce", cn, sizeof(cn));
            do_auth(h, c, s, t);
            if (c->state == C_AUTHED) welcome(h, c, cn, nonce);
        }
        else if (!strcmp(type, "pair")) do_pair(h, c, s, t);
        else send_error(c, "auth", "authentication required");
        free(t);
        return;
    }
    if (c->state != C_AUTHED)
    {
        free(t);
        return;
    }
    c->last_rx = h->now;
    if (h->agent == c)
    {
        h->last_seen = h->now;
        h->last_seen_wall = wall();
    }
    int data = json_get(s, t, 0, "data");
    if (!strcmp(type, "inventory") && data > 0 && t[data].type == J_OBJ)
    {
        store(&h->inventory, s, t, data);
        h->inventory_at = h->now;
        h->gen++;
    }
    else if (!strcmp(type, "metrics") && data > 0 && t[data].type == J_OBJ) on_metrics(h, s, t, data);
    else if (!strcmp(type, "event") && data > 0 && t[data].type == J_OBJ) on_host_event(h, s, t, data);
    else if (!strcmp(type, "result")) on_result(h, s, t);
    /* pong: only the time; anything else: a newer agent's, ignored */
    free(t);
}

void hl_conn_input(struct hl *h, struct hl_conn *c, const char *data, size_t n, int64_t now)
{
    h->now = now;
    if (c->close_why[0]) return;
    buf_add(&c->in, data, n);
    if (c->in.oom) return (void)snprintf(c->close_why, sizeof(c->close_why), "out of memory");
    size_t limit = c->state == C_AUTHED ? HL_MAX_LINE : HL_PRE_LINE;
    for (;;)
    {
        char *nl = c->in.len ? memchr(c->in.s, '\n', c->in.len) : NULL;
        if (!nl) break;
        size_t l = nl - c->in.s;
        if (l > limit)
        {
            snprintf(c->close_why, sizeof(c->close_why), "line too long");
            c->state = C_CLOSING;
            return;
        }
        line(h, c, c->in.s, l);
        buf_consume(&c->in, l + 1);
        if (c->close_why[0]) return;
        limit = c->state == C_AUTHED ? HL_MAX_LINE : HL_PRE_LINE;
    }
    if (c->in.len > limit)
    {
        snprintf(c->close_why, sizeof(c->close_why), "line too long");
        c->state = C_CLOSING;
    }
}

/* ------------------------------------------------------------------ time */
void hl_tick(struct hl *h, int64_t now)
{
    h->now = now;
    if (h->agent)
    {
        struct hl_conn *a = h->agent;
        if (now - h->last_ping >= h->c.ping_ms)
        {
            h->last_ping = now;
            char l[48];
            snprintf(l, sizeof(l), "{\"v\":%d,\"t\":\"ping\"}", HL_PROTO);
            send_raw(a, l);
        }
        /* a half-open connection (the host reset): dropped */
        if (now - a->last_rx > h->c.lost_ms && !a->close_why[0])
        {
            snprintf(a->close_why, sizeof(a->close_why), "silent for %d s", h->c.lost_ms / 1000);
            a->state = C_CLOSING;
        }
    }
    else if (h->ever && h->gone_at && !h->lost_reported && now - h->gone_at >= h->c.lost_ms)
    {
        h->lost_reported = 1;
        hl_event(h, 4, "agent_lost", "host agent lost: no connection for %d s", h->c.lost_ms / 1000);
    }
    if (h->metrics_at && now - h->metrics_at > h->c.lost_ms)
    {
        int had = 0;
        for (int i = 0; i < NSENS; i++)
        {
            had |= !isnan(h->sens[i]);
            h->sens[i] = NAN;
        }
        if (had) h->sens_gen++, h->gen++;
    }
    for (int i = 0; i < h->nres; i++)
    {
        struct hl_result *r = &h->res[i];
        if (!r->done && now - r->sent > h->c.control_timeout_ms)
        {
            r->done = 1;
            r->ok = 0;
            snprintf(r->text, sizeof(r->text), "no answer from the agent");
            hl_event(h, 4, "control", "host: %s: no answer from the agent", r->op);
        }
    }
    if (h->turbo_until && now >= h->turbo_until)
    {
        h->turbo_until = 0;
        h->fand_turbo = 0;
        hl_log(5, "GPU fan turbo over");
        push_policy(h);
    }
    if (h->token[0] && now > h->token_until)
    {
        OPENSSL_cleanse(h->token, sizeof(h->token));
        hl_log(5, "pairing expired");
        h->gen++;
    }
}

/* ------------------------------------------------------------------ requests */
int hl_set_gpu_fan(struct hl *h, const char *mode, double duty, char *err, size_t n)
{
    if (!strcmp(mode, "auto")) duty = 0;
    else if (!strcmp(mode, "manual"))
    {
        if (!(duty >= 20 && duty <= 100)) return snprintf(err, n, "duty must be 20..100 %% (the host keeps its own floor)"), -1;
    }
    else return snprintf(err, n, "mode must be auto or manual"), -1;
    snprintf(h->fan_mode, sizeof(h->fan_mode), "%s", mode);
    h->fan_duty = duty;
    save_policy(h);
    if (duty) hl_log(5, "GPU fan policy: manual %g %%", duty);
    else hl_log(5, "GPU fan policy: auto");
    push_policy(h);
    return 0;
}

int hl_gpu_turbo(struct hl *h, int minutes, char *err, size_t n)
{
    if (minutes < 0 || minutes > 120) return snprintf(err, n, "minutes must be 0..120"), -1;
    h->turbo_until = minutes ? h->now + (int64_t)minutes * 60000 : 0;
    h->fand_turbo = 0;
    hl_log(5, "GPU fan turbo %s", minutes ? "on" : "off");
    push_policy(h);
    return 0;
}

void hl_fand_turbo(struct hl *h, int secs)
{
    if (!h->c.follow_fand_turbo) return;
    if (secs > 0)
    {
        int64_t until = h->now + (int64_t)secs * 1000;
        if (!h->fand_turbo && h->turbo_until > until) return; /* ours is longer */
        if (h->fand_turbo && llabs(h->turbo_until - until) < 5000) return;
        h->turbo_until = until;
        h->fand_turbo = 1;
        hl_log(5, "GPU fan turbo with the BMC fans' (%d s)", secs);
        push_policy(h);
    }
    else if (h->fand_turbo)
    {
        h->turbo_until = 0;
        h->fand_turbo = 0;
        push_policy(h);
    }
}

static int control(struct hl *h, const char *op, const char *extra, char *err, size_t n)
{
    if (!h->agent) return snprintf(err, n, "host agent not connected"), -1;
    struct hl_result *r = &h->res[h->respos];
    h->respos = (h->respos + 1) % HL_MAX_RESULTS;
    if (h->nres < HL_MAX_RESULTS) h->nres++;
    memset(r, 0, sizeof(*r));
    r->id = h->next_id++;
    snprintf(r->op, sizeof(r->op), "%s", op);
    r->sent = h->now;
    char l[200];
    snprintf(l, sizeof(l), "{\"v\":%d,\"t\":\"control\",\"id\":%d,\"op\":\"%s\"%s}", HL_PROTO, r->id, op, extra);
    send_raw(h->agent, l);
    h->gen++;
    return 0;
}

int hl_os_power(struct hl *h, const char *action, char *err, size_t n)
{
    if (strcmp(action, "poweroff") && strcmp(action, "reboot")) return snprintf(err, n, "action must be poweroff or reboot"), -1;
    char extra[40];
    snprintf(extra, sizeof(extra), ",\"action\":\"%s\"", action);
    if (control(h, "os_power", extra, err, n)) return -1;
    hl_event(h, 5, "os_power", "host OS %s requested", action);
    return 0;
}

int hl_edac_load(struct hl *h, char *err, size_t n)
{
    return control(h, "edac_load", "", err, n);
}

int hl_request_inventory(struct hl *h, char *err, size_t n)
{
    if (!h->agent) return snprintf(err, n, "host agent not connected"), -1;
    char l[48];
    snprintf(l, sizeof(l), "{\"v\":%d,\"t\":\"inventory?\"}", HL_PROTO);
    send_raw(h->agent, l);
    return 0;
}

static void b64url(const unsigned char *in, size_t n, struct buf *b)
{
    static const char a[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    for (size_t i = 0; i < n; i += 3)
    {
        unsigned v = in[i] << 16 | (i + 1 < n ? in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        char o[4] = { a[v >> 18 & 63], a[v >> 12 & 63], a[v >> 6 & 63], a[v & 63] };
        buf_add(b, o, i + 2 < n ? 4 : i + 1 < n ? 3 : 2);
    }
}

/* the EUI-64 link-local address of a MAC */
static void ll_of(const char *mac, char *out, size_t n)
{
    unsigned m[6];
    out[0] = 0;
    if (sscanf(mac, "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6) return;
    snprintf(out, n, "fe80::%x:%x:%x:%x", ((m[0] ^ 2) << 8) | m[1], (m[2] << 8) | 0xff, 0xfe00 | m[3], (m[4] << 8) | m[5]);
}

/* curl's --pinnedpubkey of a PEM certificate (the first one in the file, a
 * key before it skipped): "sha256//" base64(SHA-256(SubjectPublicKeyInfo));
 * "" when there is none */
static void spki_pin(const char *file, char out[64])
{
    out[0] = 0;
    FILE *f = fopen(file, "r");
    if (!f) return;
    X509 *x = PEM_read_X509(f, NULL, NULL, NULL);
    fclose(f);
    if (!x) return;
    unsigned char *der = NULL, md[SHA256_DIGEST_LENGTH];
    int l = i2d_PUBKEY(X509_get0_pubkey(x), &der);
    if (l > 0)
    {
        SHA256(der, (size_t)l, md);
        memcpy(out, "sha256//", 8);
        EVP_EncodeBlock((unsigned char *)out + 8, md, sizeof(md));
    }
    OPENSSL_free(der);
    X509_free(x);
}

int hl_pair_begin(struct hl *h, struct buf *out, char *err, size_t n)
{
    unsigned char r[16];
    if (hl_random(r, sizeof(r))) return snprintf(err, n, "no random numbers"), -1;
    hl_hex(r, sizeof(r), h->token);
    OPENSSL_cleanse(r, sizeof(r));
    h->token_until = h->now + h->c.pair_ttl_ms;
    h->token_tries = 0;
    struct buf p = { 0 }, s = { 0 };
    buf_printf(&p, "{\"token\":\"%s\",\"fp\":\"%s\",\"mac\":\"%s\",\"v4\":\"%s\",\"port\":%d}", h->token, h->fingerprint, h->mac,
               h->ipv4, h->c.port);
    buf_add(&s, "reimu1:", 7);
    b64url((const unsigned char *)p.s, p.len, &s);
    char ll[48];
    ll_of(h->mac, ll, sizeof(ll));
    out->len = 0;
    char pin[64];
    spki_pin(h->c.https_cert, pin);
    buf_printf(out, "{\"pair\":\"%s\",\"command\":\"sudo ./reimu-hostd.py install --pair '%s'\",\"expires\":%d,"
                    "\"fingerprint\":\"%s\",\"mac\":\"%s\",\"link_local\":\"%s\",\"ipv4\":\"%s\",\"port\":%d,"
                    "\"https_pin\":\"%s\"}",
               s.s, s.s, h->c.pair_ttl_ms / 1000, h->fingerprint, h->mac, ll, h->ipv4, h->c.port, pin);
    snprintf(h->pair_str, sizeof(h->pair_str), "%s", s.s ? s.s : "");
    OPENSSL_cleanse(p.s, p.len);
    buf_free(&p);
    buf_free(&s);
    hl_event(h, 5, "pairing", "pairing of a host agent open for %d min", h->c.pair_ttl_ms / 60000);
    return out->oom ? (snprintf(err, n, "out of memory"), -1) : 0;
}

int hl_disk_begin(struct hl *h, struct buf *out, char *err, size_t n)
{
    if (hl_pair_begin(h, out, err, n)) return -1;
    if (!out->len || out->s[out->len - 1] != '}' || !h->pair_str[0]) return snprintf(err, n, "no pairing string"), -1;
    out->len--;
    buf_printf(out, ",\"disk\":true}");
    h->disk_on = 1;
    h->disk_since = h->now;
    h->disk_until = h->token_until;
    h->gen++;
    return out->oom ? (snprintf(err, n, "out of memory"), -1) : 0;
}

int hl_disk_due(const struct hl *h)
{
    if (!h->disk_on) return 0;
    if (h->now >= h->disk_until) return 1;
    /* installed: paired since the disk came, and the agent's service in */
    return h->paired_at && h->paired_at >= h->disk_since && h->authed_at >= h->paired_at;
}

void hl_disk_end(struct hl *h)
{
    h->disk_on = 0;
    h->disk_until = 0;
    OPENSSL_cleanse(h->pair_str, sizeof(h->pair_str));
    h->gen++;
}

void hl_unpair(struct hl *h)
{
    if (h->dir[0])
    {
        char p[256];
        path_of(h, "key", p, sizeof(p));
        unlink(p);
    }
    OPENSSL_cleanse(h->key, sizeof(h->key));
    OPENSSL_cleanse(h->token, sizeof(h->token));
    OPENSSL_cleanse(h->pair_str, sizeof(h->pair_str));
    h->paired = 0;
    if (h->agent)
    {
        snprintf(h->agent->close_why, sizeof(h->agent->close_why), "unpaired");
        h->agent->state = C_CLOSING;
    }
    hl_event(h, 5, "unpaired", "host agent unpaired: its key no longer works");
}

/* ------------------------------------------------------------------ state JSON */
static void jnum(struct buf *b, double v)
{
    if (isnan(v)) buf_printf(b, "null");
    else buf_printf(b, "%g", v);
}

void hl_json(struct hl *h, struct buf *b)
{
    static const char *const tn[] = { "ipv4", "ipv6-ll", "ipv6" };
    buf_printf(b, "{\"version\":\"%s\",\"port\":%d,\"iface\":", HL_VERSION, h->c.port);
    buf_json_str(b, h->c.iface);
    buf_printf(b, ",\"fingerprint\":\"%s\",\"mac\":\"%s\",\"ipv4\":\"%s\",\"paired\":%s,\"pairing\":{\"active\":%s,\"left\":%d},",
               h->fingerprint, h->mac, h->ipv4, h->paired ? "true" : "false", h->token[0] ? "true" : "false",
               h->token[0] && h->token_until > h->now ? (int)((h->token_until - h->now) / 1000) : 0);
    buf_printf(b, "\"disk\":{\"on\":%s,\"left\":%d},", h->disk_on ? "true" : "false",
               h->disk_on && h->disk_until > h->now ? (int)((h->disk_until - h->now) / 1000) : 0);
    buf_printf(b, "\"agent\":{\"connected\":%s,\"id\":", h->agent ? "true" : "false");
    buf_json_str(b, h->agent_id);
    buf_printf(b, ",\"host\":");
    buf_json_str(b, h->agent_host);
    buf_printf(b, ",\"version\":");
    buf_json_str(b, h->agent_ver);
    buf_printf(b, ",\"transport\":\"%s\",\"peer\":", h->ever ? tn[h->agent_transport] : "");
    buf_json_str(b, h->agent_peer);
    buf_printf(b, ",\"since\":%lld,\"last_seen\":%lld},", (long long)h->agent_since_wall, (long long)h->last_seen_wall);
    buf_printf(b, "\"heartbeat\":{\"state\":\"%s\",\"age\":%lld},", hl_heartbeat(h),
               h->ever ? (long long)((h->now - h->last_seen) / 1000) : -1LL);
    buf_printf(b, "\"policy\":");
    policy_json(h, b);
    buf_printf(b, ",\"fand_turbo\":%s,\"sensors\":{", h->fand_turbo ? "true" : "false");
    for (int i = 0; i < NSENS; i++)
    {
        buf_printf(b, "%s\"%s\":", i ? "," : "", hl_sensors[i].name);
        jnum(b, h->sens[i]);
    }
    buf_printf(b, "},\"thresholds\":{");
    for (int i = 0; i < 3; i++) buf_printf(b, "%s\"%s\":[%g,%g]", i ? "," : "", temp_name[i], h->c.warn[i], h->c.crit[i]);
    buf_printf(b, "},\"edac\":{\"ce\":%lld,\"ue\":%lld},", h->ce, h->ue);
    buf_printf(b, "\"inventory\":%s,\"inventory_age\":%lld,", h->inventory.len ? h->inventory.s : "null",
               h->inventory_at ? (long long)((h->now - h->inventory_at) / 1000) : -1LL);
    buf_printf(b, "\"metrics\":%s,\"metrics_age\":%lld,", h->metrics.len ? h->metrics.s : "null",
               h->metrics_at ? (long long)((h->now - h->metrics_at) / 1000) : -1LL);
    buf_printf(b, "\"events\":[");
    for (int i = 0; i < h->nev; i++)
    {
        const struct hl_event *e = &h->ev[(h->evpos - h->nev + i + HL_MAX_EVENTS) % HL_MAX_EVENTS];
        static const char *const sev[8] = { "critical", "critical", "critical", "error", "warning", "notice", "info", "debug" };
        buf_printf(b, "%s{\"time\":%lld,\"sev\":\"%s\",\"kind\":\"%s\",\"text\":", i ? "," : "", (long long)e->wall, sev[e->prio & 7],
                   e->kind);
        buf_json_str(b, e->text);
        buf_printf(b, "}");
    }
    buf_printf(b, "],\"results\":[");
    for (int i = 0; i < h->nres; i++)
    {
        const struct hl_result *r = &h->res[(h->respos - h->nres + i + HL_MAX_RESULTS) % HL_MAX_RESULTS];
        buf_printf(b, "%s{\"id\":%d,\"op\":\"%s\",\"done\":%s,\"ok\":%s,\"text\":", i ? "," : "", r->id, r->op,
                   r->done ? "true" : "false", r->ok ? "true" : "false");
        buf_json_str(b, r->text);
        buf_printf(b, "}");
    }
    buf_printf(b, "]}");
}
