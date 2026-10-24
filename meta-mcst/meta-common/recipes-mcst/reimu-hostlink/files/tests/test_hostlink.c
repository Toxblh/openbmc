/* reimu-hostlink unit tests: the JSON reader, the config, the protocol and
 * the state (hl-proto.c) driven by lines, with simulated time; no sockets.
 * Native: make test */
#define _GNU_SOURCE
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "hostlink.h"

static int s_fail, s_pass;
static char D[200]; /* state dir */
static int64_t T;

#define CHECK(c, ...)                                                   \
    do {                                                                \
        if (c) s_pass++;                                                \
        else {                                                          \
            s_fail++;                                                   \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);               \
            printf(__VA_ARGS__);                                        \
            printf("\n");                                               \
        }                                                               \
    } while (0)

/* every log line, to check that no secret is in them */
static struct buf s_logs;
static void sink(int prio, const char *line)
{
    buf_printf(&s_logs, "%d %s\n", prio, line);
}

static struct hl_event s_last_ev[64];
static int s_nev;
static void on_ev(struct hl *h, const struct hl_event *e)
{
    if (s_nev < 64) s_last_ev[s_nev++] = *e;
}
static int events_of(const char *kind, int prio)
{
    int n = 0;
    for (int i = 0; i < s_nev; i++)
        if (!strcmp(s_last_ev[i].kind, kind) && (prio < 0 || s_last_ev[i].prio == prio)) n++;
    return n;
}

/* ------------------------------------------------------------------ helpers */
static struct hl H;
static const unsigned char KEY[32] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
                                       16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31 };

static void fresh(const char *conf)
{
    struct hl_config c;
    hl_config_defaults(&c);
    char err[8][128];
    if (conf) hl_config_parse(&c, conf, err, 8);
    char cmd[300];
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", D, D);
    if (system(cmd)) abort();
    hl_init(&H, &c, D);
    H.on_event = on_ev;
    s_nev = 0;
    T = 1000000;
    hl_tick(&H, T); /* the daemon's loop keeps h->now */
}

static void write_key(const unsigned char *k)
{
    char p[300], hex[65];
    snprintf(p, sizeof(p), "%s/key", D);
    hl_hex(k, 32, hex);
    FILE *f = fopen(p, "w");
    fprintf(f, "%s\n", hex);
    fclose(f);
    chmod(p, 0600);
    hl_init(&H, &H.c, D);
    H.on_event = on_ev;
    hl_tick(&H, T);
}

/* the last complete line in conn->out of type t (copied), "" none */
static char s_line[HL_MAX_LINE + 64];
static const char *last_of(struct hl_conn *c, const char *t)
{
    s_line[0] = 0;
    char want[40];
    snprintf(want, sizeof(want), "\"t\":\"%s\"", t);
    const char *p = c->out.s;
    while (p && *p)
    {
        const char *e = strchr(p, '\n');
        if (!e) break;
        if (memmem(p, e - p, want, strlen(want)) && (size_t)(e - p) < sizeof(s_line))
        {
            memcpy(s_line, p, e - p);
            s_line[e - p] = 0;
        }
        p = e + 1;
    }
    return s_line;
}

static int count_of(struct hl_conn *c, const char *t)
{
    char want[40];
    snprintf(want, sizeof(want), "\"t\":\"%s\"", t);
    int n = 0;
    for (const char *p = c->out.s; p && (p = strstr(p, want)); p++) n++;
    return n;
}

/* a string field of a JSON line */
static char s_field[256];
static const char *field(const char *line, const char *path)
{
    struct jtok t[256];
    s_field[0] = 0;
    int n = json_parse(line, strlen(line), t, 256);
    if (n < 0) return s_field;
    int i = json_get(line, t, 0, path);
    if (i < 0) return s_field;
    if (t[i].type == J_STR) json_str(line, t, i, s_field, sizeof(s_field));
    else snprintf(s_field, sizeof(s_field), "%.*s", t[i].end - t[i].start, line + t[i].start);
    return s_field;
}

static void send_line(struct hl_conn *c, const char *fmt, ...)
{
    char b[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof(b) - 1, fmt, ap);
    va_end(ap);
    b[n++] = '\n';
    hl_conn_input(&H, c, b, n, T);
}

static void auth_line(char *out, size_t n, const unsigned char *key, const char *nonce, const char *cnonce, const char *id)
{
    char msg[300], mac[65];
    snprintf(msg, sizeof(msg), "reimu-hostlink-auth-v1\n%s\n%s\n%s", nonce, cnonce, id);
    hl_hmac_hex(key, 32, msg, mac);
    snprintf(out, n, "{\"v\":1,\"t\":\"auth\",\"agent\":\"reimu-hostd\",\"ver\":\"1.0\",\"id\":\"%s\",\"cnonce\":\"%s\",\"mac\":\"%s\",\"host\":\"elbrus\",\"transport\":\"ipv6-ll\",\"future\":{\"x\":[1,2]}}",
             id, cnonce, mac);
}

static const char *CN = "ffeeddccbbaa99887766554433221100";

/* an authenticated agent connection */
static struct hl_conn *agent(const char *peer)
{
    struct hl_conn *c = calloc(1, sizeof(*c));
    hl_conn_open(&H, c, peer, T);
    char l[1024];
    auth_line(l, sizeof(l), KEY, c->nonce, CN, "host-1");
    send_line(c, "%s", l);
    return c;
}

static void drop(struct hl_conn *c)
{
    hl_conn_closed(&H, c, T);
    hl_conn_free(c);
    free(c);
}

static const char *METRICS =
    "{\"v\":1,\"t\":\"metrics\",\"data\":{\"uptime\":100,\"load\":[0.5,0.4,0.3],"
    "\"edac\":{\"loaded\":true,\"ce_total\":%d,\"ue_total\":%d,\"mcs\":[{\"mc\":0,\"ce\":%d,\"ue\":%d,"
    "\"dimms\":[{\"dimm\":0,\"label\":\"DIMM0\",\"ce\":%d,\"ue\":%d},{\"dimm\":1,\"label\":\"DIMM1\",\"ce\":0,\"ue\":0}]}]},"
    "\"gpu\":{\"slot\":\"0000:03:00.0\",\"temp_edge\":%g,\"temp_junction\":%g,\"temp_mem\":58,\"fan_rpm\":1200,"
    "\"pwm_pct\":40.0,\"power_w\":35.5,\"policy\":{\"mode\":\"manual\",\"duty\":40}},"
    "\"disks\":[{\"name\":\"nvme0n1\",\"temp\":41.85},{\"name\":\"sda\",\"temp\":35},{\"name\":\"sdb\",\"temp\":null}],"
    "\"agent\":{\"version\":\"1.0\",\"interval\":10}}}";

static void metrics(struct hl_conn *c, int ce, int ue, double edge, double junction)
{
    send_line(c, METRICS, ce, ue, ce, ue, ce, ue, edge, junction);
}

/* ------------------------------------------------------------------ tests */
static void t_json(void)
{
    struct jtok t[64];
    const char *s = " {\"a\":{\"b\":[1,2.5,{\"c\":\"x\\ny\\u00e9\"}]},\"t\":true,\"f\":false,\"n\":null,\"e\":-1e3} ";
    int n = json_parse(s, strlen(s), t, 64);
    CHECK(n > 0, "parse: %d", n);
    int b = json_get(s, t, 0, "a.b");
    CHECK(b > 0 && t[b].type == J_ARR && t[b].size == 3, "a.b array of 3");
    double v = 0;
    CHECK(json_num(s, t, json_item(t, b, 1), &v) == 0 && v == 2.5, "a.b[1] = 2.5 (%g)", v);
    char out[32];
    int c = json_get(s, t, json_item(t, b, 2), "c");
    CHECK(json_str(s, t, c, out, sizeof(out)) == 0 && !strcmp(out, "x\ny\xc3\xa9"), "unescape: %s", out);
    int bv = -1;
    CHECK(json_bool(t, json_get(s, t, 0, "t"), &bv) == 0 && bv == 1, "true");
    CHECK(json_bool(t, json_get(s, t, 0, "f"), &bv) == 0 && bv == 0, "false");
    CHECK(t[json_get(s, t, 0, "n")].type == J_NULL, "null");
    CHECK(json_num(s, t, json_get(s, t, 0, "e"), &v) == 0 && v == -1000, "-1e3");
    CHECK(json_get(s, t, 0, "a.x") < 0 && json_get(s, t, 0, "zz") < 0, "missing paths");
    CHECK(json_item(t, b, 3) < 0, "item out of range");
    CHECK(json_str(s, t, json_get(s, t, 0, "e"), out, sizeof(out)) < 0, "a number is no string");

    const char *bad[] = { "", "{", "{\"a\":1,}", "{\"a\" 1}", "[1,2", "{\"a\":\"x}", "{} x", "{\"a\":01}",
                          "{\"a\":\"\x01\"}", "nul", "{\"a\":[1,]}", "{\"a\":1e}", "{\"a\":\"\\q\"}" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        CHECK(json_parse(bad[i], strlen(bad[i]), t, 64) < 0, "malformed refused: %s", bad[i]);
    char deep[100] = "";
    for (int i = 0; i < 20; i++) strcat(deep, "[");
    for (int i = 0; i < 20; i++) strcat(deep, "]");
    CHECK(json_parse(deep, strlen(deep), t, 64) < 0, "too deep refused");
    CHECK(json_parse("[1,2,3,4,5]", 11, t, 4) < 0, "too many tokens refused");
    const char *nul = "{\"a\":\"x\\u0000y\"}";
    n = json_parse(nul, strlen(nul), t, 64);
    CHECK(n > 0 && json_str(nul, t, json_get(nul, t, 0, "a"), out, sizeof(out)) < 0, "\\u0000 refused");

    struct buf w = { 0 };
    buf_json_str(&w, "a\"b\\c\n\x01é");
    CHECK(w.s && !strcmp(w.s, "\"a\\\"b\\\\c\\n\\u0001é\""), "escape: %s", w.s);
    buf_free(&w);
}

static void t_config(void)
{
    struct hl_config c;
    hl_config_defaults(&c);
    CHECK(c.port == 5564 && !strcmp(c.iface, "eth0") && c.allow_ipv4 && c.lost_ms == 60000, "defaults");
    char err[8][128];
    int n = hl_config_parse(&c, "# x\nPORT=6000\nIFACE=\"eth1\"\nALLOW_IPV4=no\nGPU_JUNCTION_WARN=90\nLOST_SEC=abc\nPORT=99999\nNEW_THING=1\n",
                            err, 8);
    CHECK(c.port == 6000 && !strcmp(c.iface, "eth1") && !c.allow_ipv4 && c.warn[1] == 90, "values");
    CHECK(n == 2 && c.lost_ms == 60000, "2 errors (%d), bad values keep the default", n);
    hl_config_parse(&c, "IFACE=\n", err, 8);
    CHECK(c.iface[0] == 0, "IFACE= : any interface");
}

static void t_hmac(void)
{
    char out[65];
    hl_hmac_hex(KEY, 32, "reimu-hostlink-auth-v1\n00112233445566778899aabbccddeeff\nffeeddccbbaa99887766554433221100\nhost-1", out);
    CHECK(!strcmp(out, "8a27b644054d9c44033e6af61345fd572d3ef9a90cab02e2c8bf2a0cb49cbaf5"), "auth vector (the agent's): %s", out);
    hl_hmac_hex(KEY, 32, "reimu-hostlink-srv-v1\nffeeddccbbaa99887766554433221100\n00112233445566778899aabbccddeeff", out);
    CHECK(!strcmp(out, "9100cb9d44c8c48187b343cefa62d490437e319e5973e7e7c03b89c8ac34df6e"), "server vector: %s", out);
}

static void t_hello(void)
{
    fresh(NULL);
    struct hl_conn c = { 0 };
    hl_conn_open(&H, &c, "fe80::1%eth0", T);
    const char *l = last_of(&c, "hello");
    CHECK(*l && !strcmp(field(l, "v"), "1") && !strcmp(field(l, "server"), "reimu-hostlink"), "hello: %s", l);
    CHECK(strlen(c.nonce) == 32 && !strcmp(field(l, "nonce"), c.nonce), "nonce in it");
    CHECK(c.transport == T_IPV6_LL, "fe80:: is link-local");
    struct hl_conn c2 = { 0 };
    hl_conn_open(&H, &c2, "::ffff:192.168.10.50", T);
    CHECK(c2.transport == T_IPV4 && strcmp(c.nonce, c2.nonce), "v4-mapped is IPv4, another nonce");
    hl_conn_free(&c);
    hl_conn_free(&c2);
}

static void t_ipv4_refused(void)
{
    fresh("ALLOW_IPV4=no\n");
    struct hl_conn c = { 0 };
    hl_conn_open(&H, &c, "::ffff:192.168.10.50", T);
    CHECK(c.close_why[0] && !*last_of(&c, "hello"), "IPv4 peer closed when ALLOW_IPV4=no (%s)", c.close_why);
    hl_conn_free(&c);
}

static void t_auth(void)
{
    fresh(NULL);
    write_key(KEY);
    struct hl_conn *c = agent("fe80::9aa7:b0ff:fe06:f61%eth0");
    CHECK(c->state == C_AUTHED && H.agent == c && !c->close_why[0], "authenticated (%s)", c->close_why);
    const char *w = last_of(c, "welcome");
    char msg[200], mac[65];
    snprintf(msg, sizeof(msg), "reimu-hostlink-srv-v1\n%s\n%s", CN, c->nonce);
    hl_hmac_hex(KEY, 32, msg, mac);
    CHECK(*w && !strcmp(field(w, "mac"), mac), "welcome proves the key");
    CHECK(*w && !strcmp(field(w, "policy.gpu_fan.mode"), "auto"), "welcome carries the policy: %s", w);
    CHECK(hl_connected(&H) && !strcmp(H.agent_id, "host-1") && !strcmp(H.agent_host, "elbrus"), "agent known");
    CHECK(events_of("agent_connected", -1) == 1, "connected event");
    drop(c);
    CHECK(!hl_connected(&H), "gone");
}

static void t_auth_fail(void)
{
    fresh(NULL);
    write_key(KEY);
    unsigned char other[32] = { 9 };
    struct hl_conn c = { 0 };
    hl_conn_open(&H, &c, "fe80::1%eth0", T);
    char l[1024];
    auth_line(l, sizeof(l), other, c.nonce, CN, "host-1");
    send_line(&c, "%s", l);
    CHECK(c.state != C_AUTHED && H.agent == NULL, "wrong key refused");
    CHECK(!strcmp(field(last_of(&c, "error"), "code"), "auth") && c.close_why[0], "error auth, closed");
    hl_conn_free(&c);
    CHECK(!strstr(s_logs.s ? s_logs.s : "", l + 80), "the MAC of the attempt not in the logs");
}

static void t_not_paired(void)
{
    fresh(NULL); /* no key */
    struct hl_conn *c = agent("fe80::1%eth0");
    CHECK(c->state != C_AUTHED && !strcmp(field(last_of(c, "error"), "code"), "auth"), "not paired: auth refused");
    hl_conn_free(c);
    free(c);
}

static void t_replay(void)
{
    fresh(NULL);
    write_key(KEY);
    struct hl_conn c = { 0 };
    hl_conn_open(&H, &c, "fe80::1%eth0", T);
    char l[1024];
    auth_line(l, sizeof(l), KEY, c.nonce, CN, "host-1");
    send_line(&c, "%s", l);
    CHECK(c.state == C_AUTHED, "first session");
    hl_conn_closed(&H, &c, T);
    hl_conn_free(&c);
    struct hl_conn c2 = { 0 };
    hl_conn_open(&H, &c2, "fe80::1%eth0", T);
    send_line(&c2, "%s", l); /* the recorded auth line again */
    CHECK(c2.state != C_AUTHED && c2.close_why[0], "replayed auth refused");
    hl_conn_free(&c2);
    /* a second auth on an authenticated connection: ignored, still the same */
    struct hl_conn *c3 = agent("fe80::1%eth0");
    send_line(c3, "%s", l);
    CHECK(c3->state == C_AUTHED && H.agent == c3, "second auth line ignored");
    drop(c3);
}

static void t_oversize(void)
{
    fresh(NULL);
    write_key(KEY);
    struct hl_conn c = { 0 };
    hl_conn_open(&H, &c, "fe80::1%eth0", T);
    char *big = malloc(HL_MAX_LINE + 10);
    memset(big, 'x', HL_MAX_LINE + 10);
    hl_conn_input(&H, &c, big, HL_PRE_LINE + 1, T);
    CHECK(c.close_why[0] && strstr(c.close_why, "long"), "before auth: %d bytes without a newline closed (%s)", HL_PRE_LINE + 1,
          c.close_why);
    hl_conn_free(&c);
    struct hl_conn *a = agent("fe80::1%eth0");
    memcpy(big, "{\"v\":1,\"t\":\"inventory\",\"data\":\"", 32);
    hl_conn_input(&H, a, big, HL_MAX_LINE + 10, T);
    CHECK(a->close_why[0] && strstr(a->close_why, "long"), "after auth: over %d closed", HL_MAX_LINE);
    drop(a);
    free(big);
}

static void t_malformed(void)
{
    fresh(NULL);
    write_key(KEY);
    struct hl_conn c = { 0 };
    hl_conn_open(&H, &c, "fe80::1%eth0", T);
    send_line(&c, "garbage {");
    CHECK(c.close_why[0] && !strcmp(field(last_of(&c, "error"), "code"), "malformed"), "malformed before auth: closed");
    hl_conn_free(&c);
    struct hl_conn c2 = { 0 };
    hl_conn_open(&H, &c2, "fe80::1%eth0", T);
    send_line(&c2, "{\"v\":1,\"t\":\"metrics\",\"data\":{}}");
    CHECK(c2.close_why[0] && !strcmp(field(last_of(&c2, "error"), "code"), "auth"), "a message before auth: closed");
    hl_conn_free(&c2);
    struct hl_conn *a = agent("fe80::1%eth0");
    for (int i = 0; i < 3; i++) send_line(a, "{\"v\":1,");
    CHECK(!a->close_why[0], "after auth a few malformed lines are skipped");
    send_line(a, "{\"v\":1,\"t\":\"something-new\",\"x\":1}");
    send_line(a, "{\"v\":1,\"t\":\"metrics\"}"); /* no data: skipped */
    CHECK(!a->close_why[0] && a->state == C_AUTHED, "unknown type and missing data ignored");
    for (int i = 0; i < 20; i++) send_line(a, "]");
    CHECK(a->close_why[0], "many malformed: closed");
    drop(a);
}

static void t_version(void)
{
    fresh(NULL);
    write_key(KEY);
    struct hl_conn c = { 0 };
    hl_conn_open(&H, &c, "fe80::1%eth0", T);
    send_line(&c, "{\"v\":2,\"t\":\"auth\",\"id\":\"x\"}");
    CHECK(c.close_why[0] && !strcmp(field(last_of(&c, "error"), "code"), "version"), "v2 refused with code version");
    hl_conn_free(&c);
    struct hl_conn c2 = { 0 };
    hl_conn_open(&H, &c2, "fe80::1%eth0", T);
    send_line(&c2, "{\"t\":\"auth\"}");
    CHECK(c2.close_why[0], "no version: refused");
    hl_conn_free(&c2);
}

static void t_rate(void)
{
    fresh(NULL);
    write_key(KEY);
    struct hl_conn *a = agent("fe80::1%eth0");
    for (int i = 0; i < 30; i++) send_line(a, "{\"v\":1,\"t\":\"pong\"}");
    CHECK(!a->close_why[0], "30 at once fine");
    for (int i = 0; i < 60; i++) send_line(a, "{\"v\":1,\"t\":\"pong\"}");
    CHECK(a->close_why[0] && strstr(a->close_why, "rate"), "a flood: closed (%s)", a->close_why);
    drop(a);
    a = agent("fe80::1%eth0");
    for (int i = 0; i < 200; i++)
    {
        T += 500; /* 2 a second: under the rate */
        send_line(a, "{\"v\":1,\"t\":\"pong\"}");
    }
    CHECK(!a->close_why[0], "steady 2/s fine");
    drop(a);
}

static void t_auth_timeout(void)
{
    fresh(NULL);
    struct hl_conn c = { 0 };
    hl_conn_open(&H, &c, "fe80::1%eth0", T);
    T += 5000;
    hl_tick(&H, T);
    CHECK(!c.close_why[0], "5 s: still waiting");
    T += 6000;
    hl_conn_tick(&H, &c, T); /* the net layer, every second, for each connection */
    CHECK(c.close_why[0] && strstr(c.close_why, "time"), "11 s without auth: closed (%s)", c.close_why);
    hl_conn_free(&c);
}

static void t_pairing(void)
{
    fresh(NULL);
    s_logs.len = 0;
    struct buf out = { 0 };
    char err[128];
    CHECK(hl_pair_begin(&H, &out, err, sizeof(err)) == 0, "pair begin");
    char tok[33];
    snprintf(tok, sizeof(tok), "%s", H.token);
    CHECK(strlen(tok) == 32 && out.s && strstr(out.s, "reimu1:"), "token and the pairing string: %s", out.s);
    CHECK(strstr(out.s, "reimu-hostd.py install --pair"), "the install command");
    struct jtok t[64];
    CHECK(json_parse(out.s, out.len, t, 64) > 0, "Pair() returns JSON");
    buf_free(&out);
    /* wrong token */
    struct hl_conn c = { 0 };
    hl_conn_open(&H, &c, "fe80::1%eth0", T);
    send_line(&c, "{\"v\":1,\"t\":\"pair\",\"token\":\"00000000000000000000000000000000\",\"id\":\"host-1\",\"host\":\"elbrus\"}");
    CHECK(!strcmp(field(last_of(&c, "error"), "code"), "pair") && c.close_why[0], "wrong token refused");
    hl_conn_free(&c);
    /* right token */
    struct hl_conn c2 = { 0 };
    hl_conn_open(&H, &c2, "fe80::1%eth0", T);
    send_line(&c2, "{\"v\":1,\"t\":\"pair\",\"token\":\"%s\",\"id\":\"host-1\",\"host\":\"elbrus\"}", tok);
    const char *p = last_of(&c2, "paired");
    CHECK(*p && strlen(field(p, "key")) == 64, "paired, a 64-hex key: %s", p);
    char key[65];
    snprintf(key, sizeof(key), "%s", field(p, "key"));
    CHECK(c2.close_why[0], "closed after pairing (the agent connects again)");
    hl_conn_free(&c2);
    char kp[300];
    snprintf(kp, sizeof(kp), "%s/key", D);
    struct stat st;
    CHECK(stat(kp, &st) == 0 && (st.st_mode & 0777) == 0600, "key file 0600");
    CHECK(H.paired && !H.token[0], "paired, the token used up");
    /* the token is one-time */
    struct hl_conn c3 = { 0 };
    hl_conn_open(&H, &c3, "fe80::1%eth0", T);
    send_line(&c3, "{\"v\":1,\"t\":\"pair\",\"token\":\"%s\",\"id\":\"host-2\"}", tok);
    CHECK(!*last_of(&c3, "paired"), "the token works once");
    hl_conn_free(&c3);
    /* the new key authenticates */
    unsigned char k[32];
    for (int i = 0; i < 32; i++) sscanf(key + 2 * i, "%2hhx", &k[i]);
    struct hl_conn c4 = { 0 };
    hl_conn_open(&H, &c4, "fe80::1%eth0", T);
    char l[1024];
    auth_line(l, sizeof(l), k, c4.nonce, CN, "host-1");
    send_line(&c4, "%s", l);
    CHECK(c4.state == C_AUTHED, "the paired key authenticates");
    /* no secret in the logs nor in the state */
    CHECK(!strstr(s_logs.s ? s_logs.s : "", tok) && !strstr(s_logs.s ? s_logs.s : "", key), "token and key not logged");
    struct buf j = { 0 };
    hl_json(&H, &j);
    CHECK(!strstr(j.s, tok) && !strstr(j.s, key), "nor in the Json");
    buf_free(&j);
    /* unpair: the agent is dropped and can't come back */
    hl_unpair(&H);
    CHECK(c4.close_why[0] && !H.paired && stat(kp, &st) != 0, "unpaired: closed, key removed");
    hl_conn_closed(&H, &c4, T);
    hl_conn_free(&c4);
    struct hl_conn c5 = { 0 };
    hl_conn_open(&H, &c5, "fe80::1%eth0", T);
    auth_line(l, sizeof(l), k, c5.nonce, CN, "host-1");
    send_line(&c5, "%s", l);
    CHECK(c5.state != C_AUTHED, "revoked key refused");
    hl_conn_free(&c5);
}

static void t_pairing_limits(void)
{
    fresh(NULL);
    struct buf out = { 0 };
    char err[128];
    hl_pair_begin(&H, &out, err, sizeof(err));
    buf_free(&out);
    char tok[33];
    snprintf(tok, sizeof(tok), "%s", H.token);
    for (int i = 0; i < 5; i++)
    {
        struct hl_conn c = { 0 };
        hl_conn_open(&H, &c, "fe80::1%eth0", T);
        send_line(&c, "{\"v\":1,\"t\":\"pair\",\"token\":\"%032d\",\"id\":\"h\"}", i);
        hl_conn_free(&c);
    }
    struct hl_conn c = { 0 };
    hl_conn_open(&H, &c, "fe80::1%eth0", T);
    send_line(&c, "{\"v\":1,\"t\":\"pair\",\"token\":\"%s\",\"id\":\"h\"}", tok);
    CHECK(!*last_of(&c, "paired") && !H.token[0], "5 wrong tries: the token is void");
    hl_conn_free(&c);
    hl_pair_begin(&H, &out, err, sizeof(err));
    buf_free(&out);
    snprintf(tok, sizeof(tok), "%s", H.token);
    T += 16 * 60 * 1000;
    struct hl_conn c2 = { 0 };
    hl_conn_open(&H, &c2, "fe80::1%eth0", T);
    send_line(&c2, "{\"v\":1,\"t\":\"pair\",\"token\":\"%s\",\"id\":\"h\"}", tok);
    CHECK(!*last_of(&c2, "paired"), "expired after 15 min");
    hl_conn_free(&c2);
}

/* bmcweb's server.pem: its key, then its certificate; the pin as curl's
 * --pinnedpubkey takes it, by openssl x509 -pubkey | openssl pkey -pubin
 * -outform der | openssl dgst -sha256 -binary | base64 */
static const char HTTPS_PEM[] =
    "-----BEGIN PRIVATE KEY-----\n"
    "MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgF0Zu71gFnKbzC3l1\n"
    "vIZ6UEI3t3z0+RwpwNVjRwvOkYShRANCAAReMuEC0JTICVe/zR3AbM/nSCo9l08K\n"
    "ecvRfXt39LISrBY0Y7UF6bwjEk/rl0bAw4gDwsKBvahaifHJ6xeAi8BX\n"
    "-----END PRIVATE KEY-----\n"
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBfjCCASOgAwIBAgIUMf63o4SpCaowzAhpZvH4puQ+MDIwCgYIKoZIzj0EAwIw\n"
    "EzERMA8GA1UEAwwIdGVzdGhvc3QwIBcNMjYxMDI0MjAxODQ1WhgPMjEyNjA5MzAy\n"
    "MDE4NDVaMBMxETAPBgNVBAMMCHRlc3Rob3N0MFkwEwYHKoZIzj0CAQYIKoZIzj0D\n"
    "AQcDQgAEXjLhAtCUyAlXv80dwGzP50gqPZdPCnnL0X17d/SyEqwWNGO1Bem8IxJP\n"
    "65dGwMOIA8LCgb2oWonxyesXgIvAV6NTMFEwHQYDVR0OBBYEFOt2DJkJZJ2k193Z\n"
    "8beiTEIezrwfMB8GA1UdIwQYMBaAFOt2DJkJZJ2k193Z8beiTEIezrwfMA8GA1Ud\n"
    "EwEB/wQFMAMBAf8wCgYIKoZIzj0EAwIDSQAwRgIhAIZsPcFyShnc30kItVDDr+ki\n"
    "MOPIwITh0/C2v6eA57w7AiEA1nECfTmCUvbICrjn+Ax31rNTNCttWkQUF/kJeSIf\n"
    "zb4=\n"
    "-----END CERTIFICATE-----\n";
#define HTTPS_PIN "sha256//3bXQ+MEFRIBczRNaJoNJApGxckXic4tjTv/Kdu2Bvqs="

static void t_https_pin(void)
{
    char conf[400], pem[300];
    snprintf(pem, sizeof(pem), "%s/a-rather-long-directory-name-for-the-certificate/server.pem", D);
    snprintf(conf, sizeof(conf), "HTTPS_CERT=%s\n", pem);
    fresh(conf);
    CHECK(!strcmp(H.c.https_cert, pem), "HTTPS_CERT: a long path kept: %s", H.c.https_cert);
    struct hl_config dc;
    hl_config_defaults(&dc);
    CHECK(!strcmp(dc.https_cert, "/etc/ssl/certs/https/server.pem"), "default: bmcweb's certificate");
    char cmd[400];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s/a-rather-long-directory-name-for-the-certificate", D);
    CHECK(system(cmd) == 0, "mkdir");
    FILE *f = fopen(pem, "w");
    fputs(HTTPS_PEM, f);
    fclose(f);
    struct buf out = { 0 };
    char err[128];
    CHECK(hl_pair_begin(&H, &out, err, sizeof(err)) == 0, "pair begin");
    CHECK(out.s && strstr(out.s, "\"https_pin\":\"" HTTPS_PIN "\""), "Pair() gives the pin of bmcweb's key: %s", out.s);
    buf_free(&out);
    unlink(pem);
    CHECK(hl_pair_begin(&H, &out, err, sizeof(err)) == 0, "no certificate: pairing still works");
    CHECK(out.s && strstr(out.s, "\"https_pin\":\"\""), "...with an empty pin: %s", out.s);
    buf_free(&out);
}

static void t_agent_disk(void)
{
    fresh(NULL);
    struct buf out = { 0 }, j = { 0 };
    char err[128];
    CHECK(!H.disk_on && !hl_disk_due(&H), "no disk at start");
    CHECK(hl_disk_begin(&H, &out, err, sizeof(err)) == 0, "disk begin");
    CHECK(out.s && strstr(out.s, "\"pair\":\"reimu1:") && strstr(out.s, "\"disk\":true"), "a pairing for the disk: %s", out.s);
    CHECK(H.disk_on && !strncmp(H.pair_str, "reimu1:", 7) && strstr(out.s, H.pair_str), "its pairing string kept for the helper");
    buf_free(&out);
    char tok[33];
    snprintf(tok, sizeof(tok), "%s", H.token);
    hl_json(&H, &j);
    CHECK(strstr(j.s, "\"disk\":{\"on\":true,\"left\":900}"), "Json: the disk on, 15 min: %s", strstr(j.s, "\"disk\"") ? strstr(j.s, "\"disk\"") : j.s);
    CHECK(!strstr(j.s, tok), "no token in the Json");
    buf_free(&j);
    T += 60000;
    hl_tick(&H, T);
    hl_json(&H, &j);
    CHECK(strstr(j.s, "\"disk\":{\"on\":true,\"left\":840}"), "a minute later: 14 min left");
    buf_free(&j);
    CHECK(!hl_disk_due(&H), "not due before the install");
    /* the installer on the host pairs ... */
    struct hl_conn c = { 0 };
    hl_conn_open(&H, &c, "fe80::1%eth0", T);
    send_line(&c, "{\"v\":1,\"t\":\"pair\",\"token\":\"%s\",\"id\":\"host-1\",\"host\":\"elbrus\"}", tok);
    const char *p = last_of(&c, "paired");
    char key[65];
    snprintf(key, sizeof(key), "%s", field(p, "key"));
    hl_conn_closed(&H, &c, T);
    hl_conn_free(&c);
    CHECK(H.paired && !hl_disk_due(&H), "paired: not yet (the installer still runs, from the disk)");
    /* ... and the agent's service connects: the disk goes */
    T += 3000;
    unsigned char k[32];
    for (int i = 0; i < 32; i++) sscanf(key + 2 * i, "%2hhx", &k[i]);
    struct hl_conn c2 = { 0 };
    hl_conn_open(&H, &c2, "fe80::1%eth0", T);
    char l[1024];
    auth_line(l, sizeof(l), k, c2.nonce, CN, "host-1");
    send_line(&c2, "%s", l);
    CHECK(c2.state == C_AUTHED && hl_disk_due(&H), "the agent connected after pairing: due");
    hl_disk_end(&H);
    CHECK(!H.disk_on && !H.pair_str[0] && !hl_disk_due(&H), "ended: its pairing string wiped");
    hl_json(&H, &j);
    CHECK(strstr(j.s, "\"disk\":{\"on\":false,\"left\":0}"), "Json: off");
    buf_free(&j);
    /* an agent already connected doesn't count: only one that paired since */
    CHECK(hl_disk_begin(&H, &out, err, sizeof(err)) == 0, "disk again");
    buf_free(&out);
    T += 1000;
    hl_tick(&H, T);
    CHECK(!hl_disk_due(&H), "the old agent's connection is no install");
    /* nobody installs: gone after the token's 15 min */
    T += 15 * 60000;
    hl_tick(&H, T);
    CHECK(hl_disk_due(&H), "15 min: due");
    hl_disk_end(&H);
    hl_conn_closed(&H, &c2, T);
    hl_conn_free(&c2);
    /* unpair wipes it too */
    hl_disk_begin(&H, &out, err, sizeof(err));
    buf_free(&out);
    hl_unpair(&H);
    CHECK(!H.pair_str[0], "unpair: the pairing string wiped");
}

static void t_inventory_metrics(void)
{
    fresh(NULL);
    write_key(KEY);
    struct hl_conn *a = agent("fe80::1%eth0");
    CHECK(isnan(H.sens[S_GPU_EDGE]), "no value before metrics");
    send_line(a, "{\"v\":1,\"t\":\"inventory\",\"data\":{\"os\":{\"name\":\"ALT\"},\"cpu\":{\"model\":\"E8C2\"},\"pci\":[]}}");
    metrics(a, 0, 0, 52, 61);
    CHECK(H.sens[S_GPU_EDGE] == 52 && H.sens[S_GPU_JUNCTION] == 61 && H.sens[S_GPU_MEM] == 58, "GPU temps");
    CHECK(H.sens[S_GPU_FAN] == 1200 && H.sens[S_GPU_PWM] == 40 && H.sens[S_GPU_POWER] == 35.5, "fan, pwm, power");
    CHECK(H.sens[S_DISK_MAX] == 41.85, "the hottest disk (%g)", H.sens[S_DISK_MAX]);
    CHECK(hl_gpu_temp(&H) == 61, "fand's input: the junction");
    struct buf j = { 0 };
    hl_json(&H, &j);
    struct jtok *t = malloc(sizeof(struct jtok) * 4096);
    int n = json_parse(j.s, j.len, t, 4096);
    CHECK(n > 0, "the Json is JSON");
    char s[64];
    CHECK(n > 0 && json_str(j.s, t, json_get(j.s, t, 0, "inventory.cpu.model"), s, sizeof(s)) == 0 && !strcmp(s, "E8C2"),
          "inventory in it");
    double v;
    CHECK(n > 0 && json_num(j.s, t, json_get(j.s, t, 0, "metrics.gpu.temp_edge"), &v) == 0 && v == 52, "metrics in it");
    int b = 0;
    CHECK(n > 0 && json_bool(t, json_get(j.s, t, 0, "agent.connected"), &b) == 0 && b, "agent.connected");
    CHECK(n > 0 && json_str(j.s, t, json_get(j.s, t, 0, "agent.transport"), s, sizeof(s)) == 0 && !strcmp(s, "ipv6-ll"),
          "transport ipv6-ll");
    CHECK(n > 0 && json_str(j.s, t, json_get(j.s, t, 0, "heartbeat.state"), s, sizeof(s)) == 0 && !strcmp(s, "ok"),
          "heartbeat ok");
    free(t);
    buf_free(&j);
    /* the GPU gone from the metrics: its sensors none */
    send_line(a, "{\"v\":1,\"t\":\"metrics\",\"data\":{\"gpu\":null,\"disks\":[]}}");
    CHECK(isnan(H.sens[S_GPU_EDGE]) && isnan(H.sens[S_DISK_MAX]), "no GPU: NAN");
    metrics(a, 0, 0, 52, 61);
    /* the agent gone: values none after the lost time */
    drop(a);
    T += 61000;
    hl_tick(&H, T);
    CHECK(isnan(H.sens[S_GPU_EDGE]), "agent lost: NAN");
}

static void t_edac_events(void)
{
    fresh(NULL);
    write_key(KEY);
    struct hl_conn *a = agent("fe80::1%eth0");
    metrics(a, 0, 0, 50, 60);
    CHECK(events_of("ecc_ce", -1) == 0, "no errors: no event");
    metrics(a, 3, 0, 50, 60);
    CHECK(events_of("ecc_ce", 4) == 1, "CE increase: warning");
    CHECK(strstr(s_last_ev[s_nev - 1].text, "DIMM0"), "names the DIMM: %s", s_last_ev[s_nev - 1].text);
    metrics(a, 3, 0, 50, 60);
    CHECK(events_of("ecc_ce", -1) == 1, "same count: no new event");
    metrics(a, 4, 1, 50, 60);
    CHECK(events_of("ecc_ue", 2) == 1 && events_of("ecc_ce", -1) == 2, "UE: critical");
    /* the BMC daemon restarts: the counts it saw are kept */
    drop(a);
    hl_init(&H, &H.c, D);
    H.on_event = on_ev;
    s_nev = 0;
    a = agent("fe80::1%eth0");
    metrics(a, 4, 1, 50, 60);
    CHECK(events_of("ecc_ce", -1) == 0 && events_of("ecc_ue", -1) == 0, "after a restart the same counts: no event");
    /* the host rebooted: counters from 0 */
    metrics(a, 0, 0, 50, 60);
    CHECK(events_of("ecc_ce", -1) == 0, "counters reset: no event");
    metrics(a, 1, 0, 50, 60);
    CHECK(events_of("ecc_ce", 4) == 1, "then an increase: warning");
    drop(a);
}

static void t_gpu_temp_events(void)
{
    fresh(NULL);
    write_key(KEY);
    struct hl_conn *a = agent("fe80::1%eth0");
    metrics(a, 0, 0, 60, 80);
    CHECK(events_of("gpu_hot", -1) == 0, "80 °C junction fine");
    metrics(a, 0, 0, 60, 96);
    CHECK(events_of("gpu_hot", 4) == 1, "96 °C junction: warning");
    metrics(a, 0, 0, 60, 97);
    CHECK(events_of("gpu_hot", -1) == 1, "still hot: no repeat");
    metrics(a, 0, 0, 60, 106);
    CHECK(events_of("gpu_hot", 2) == 1, "106 °C: critical");
    metrics(a, 0, 0, 60, 92);
    CHECK(events_of("gpu_temp_ok", -1) == 0, "92: within hysteresis, no all-clear");
    metrics(a, 0, 0, 60, 85);
    CHECK(events_of("gpu_temp_ok", 5) == 1, "85: back to normal");
    drop(a);
}

static void t_lost_back(void)
{
    fresh(NULL);
    write_key(KEY);
    CHECK(!strcmp(hl_heartbeat(&H), "never"), "never seen");
    struct hl_conn *a = agent("fe80::1%eth0");
    CHECK(!strcmp(hl_heartbeat(&H), "ok"), "ok");
    T += 35000;
    hl_tick(&H, T);
    CHECK(!strcmp(hl_heartbeat(&H), "late"), "35 s silent: late");
    drop(a);
    T += 30000;
    hl_tick(&H, T);
    CHECK(events_of("agent_lost", -1) == 0, "30 s after the drop: not lost yet");
    T += 31000;
    hl_tick(&H, T);
    CHECK(events_of("agent_lost", 4) == 1 && !strcmp(hl_heartbeat(&H), "lost"), "61 s: lost (warning)");
    T += 60000;
    hl_tick(&H, T);
    CHECK(events_of("agent_lost", -1) == 1, "once");
    a = agent("fe80::1%eth0");
    CHECK(events_of("agent_back", 5) == 1, "back (notice)");
    drop(a);
    /* a short drop (BMC side reconnect) is no loss */
    a = agent("fe80::1%eth0");
    drop(a);
    T += 5000;
    a = agent("fe80::1%eth0");
    T += 70000;
    send_line(a, "{\"v\":1,\"t\":\"pong\"}");
    hl_tick(&H, T);
    CHECK(events_of("agent_lost", -1) == 1, "short drop: no loss");
    drop(a);
}

static void t_ping(void)
{
    fresh(NULL);
    write_key(KEY);
    struct hl_conn *a = agent("fe80::1%eth0");
    int n0 = count_of(a, "ping");
    T += 10001;
    hl_tick(&H, T);
    CHECK(count_of(a, "ping") == n0 + 1, "a ping after 10 s");
    drop(a);
}

static void t_replace(void)
{
    fresh(NULL);
    write_key(KEY);
    struct hl_conn *a = agent("fe80::1%eth0");
    struct hl_conn *b = agent("::ffff:192.168.10.50");
    CHECK(H.agent == b && a->close_why[0] && strstr(a->close_why, "replaced"), "the new connection replaces the old");
    CHECK(H.agent_transport == T_IPV4, "now over IPv4");
    hl_conn_closed(&H, a, T); /* the old one closing doesn't drop the new */
    CHECK(H.agent == b, "still the new one");
    hl_conn_free(a);
    free(a);
    drop(b);
}

static void t_policy(void)
{
    fresh(NULL);
    write_key(KEY);
    char err[128];
    CHECK(hl_set_gpu_fan(&H, "manual", 45, err, sizeof(err)) == 0, "manual 45 while offline: kept");
    struct hl_conn *a = agent("fe80::1%eth0");
    const char *w = last_of(a, "welcome");
    CHECK(!strcmp(field(w, "policy.gpu_fan.mode"), "manual") && !strcmp(field(w, "policy.gpu_fan.duty"), "45"),
          "welcome carries it: %s", w);
    CHECK(hl_set_gpu_fan(&H, "manual", 60, err, sizeof(err)) == 0, "manual 60");
    const char *p = last_of(a, "policy");
    CHECK(*p && !strcmp(field(p, "gpu_fan.duty"), "60") && !strcmp(field(p, "turbo_left"), "0"), "pushed: %s", p);
    CHECK(hl_set_gpu_fan(&H, "manual", 10, err, sizeof(err)) < 0, "10 %% refused: %s", err);
    CHECK(hl_set_gpu_fan(&H, "manual", 101, err, sizeof(err)) < 0, "101 %% refused");
    CHECK(hl_set_gpu_fan(&H, "jet", 50, err, sizeof(err)) < 0, "bad mode refused");
    CHECK(hl_set_gpu_fan(&H, "auto", 0, err, sizeof(err)) == 0 && !strcmp(field(last_of(a, "policy"), "gpu_fan.mode"), "auto"),
          "auto");
    hl_set_gpu_fan(&H, "manual", 50, err, sizeof(err));
    drop(a);
    /* persisted */
    hl_init(&H, &H.c, D);
    CHECK(!strcmp(H.fan_mode, "manual") && H.fan_duty == 50, "kept across a restart of the daemon");
}

static void t_turbo(void)
{
    fresh(NULL);
    write_key(KEY);
    char err[128];
    struct hl_conn *a = agent("fe80::1%eth0");
    CHECK(hl_gpu_turbo(&H, 5, err, sizeof(err)) == 0, "turbo 5 min");
    CHECK(!strcmp(field(last_of(a, "policy"), "turbo_left"), "300"), "pushed 300 s");
    CHECK(hl_gpu_turbo(&H, 121, err, sizeof(err)) < 0, "121 min refused");
    T += 100000;
    struct hl_conn *b = agent("fe80::1%eth0"); /* reconnect during the turbo */
    CHECK(!strcmp(field(last_of(b, "welcome"), "policy.turbo_left"), "200"), "welcome: 200 s left");
    T += 201000;
    send_line(b, "{\"v\":1,\"t\":\"pong\"}"); /* the agent talks every 10 s */
    hl_tick(&H, T);
    CHECK(!strcmp(field(last_of(b, "policy"), "turbo_left"), "0") && !H.turbo_until, "expired: policy without turbo");
    hl_gpu_turbo(&H, 5, err, sizeof(err));
    CHECK(hl_gpu_turbo(&H, 0, err, sizeof(err)) == 0 && !H.turbo_until, "turbo 0: off");
    /* fand's turbo */
    hl_fand_turbo(&H, 600);
    CHECK(H.turbo_until == T + 600000 && !strcmp(field(last_of(b, "policy"), "turbo_left"), "600"), "fand turbo follows");
    hl_fand_turbo(&H, 600);
    CHECK(count_of(b, "policy") >= 1, "no flood on repeats");
    hl_fand_turbo(&H, 0);
    CHECK(!H.turbo_until, "fand turbo off: ours off");
    hl_gpu_turbo(&H, 10, err, sizeof(err));
    hl_fand_turbo(&H, 0);
    CHECK(H.turbo_until, "fand turbo off doesn't cancel a turbo set here");
    hl_conn_closed(&H, a, T);
    hl_conn_free(a);
    free(a);
    drop(b);
    fresh("FOLLOW_FAND_TURBO=no\n");
    hl_fand_turbo(&H, 600);
    CHECK(!H.turbo_until, "FOLLOW_FAND_TURBO=no");
}

static void t_control(void)
{
    fresh(NULL);
    write_key(KEY);
    char err[128];
    CHECK(hl_os_power(&H, "reboot", err, sizeof(err)) < 0 && strstr(err, "not connected"), "offline: refused (%s)", err);
    struct hl_conn *a = agent("fe80::1%eth0");
    CHECK(hl_os_power(&H, "reboot; rm -rf /", err, sizeof(err)) < 0, "bad action refused");
    CHECK(hl_os_power(&H, "reboot", err, sizeof(err)) == 0, "reboot sent");
    const char *c = last_of(a, "control");
    CHECK(!strcmp(field(c, "op"), "os_power") && !strcmp(field(c, "action"), "reboot"), "control message: %s", c);
    int id = atoi(field(c, "id"));
    send_line(a, "{\"v\":1,\"t\":\"result\",\"id\":%d,\"op\":\"os_power\",\"ok\":true,\"text\":\"systemctl reboot\"}", id);
    const struct hl_result *r = &H.res[(H.respos + HL_MAX_RESULTS - 1) % HL_MAX_RESULTS];
    CHECK(r->id == id && r->done && r->ok, "result recorded");
    CHECK(hl_edac_load(&H, err, sizeof(err)) == 0 && !strcmp(field(last_of(a, "control"), "op"), "edac_load"), "edac_load");
    T += 31000;
    hl_tick(&H, T);
    r = &H.res[(H.respos + HL_MAX_RESULTS - 1) % HL_MAX_RESULTS];
    CHECK(r->done && !r->ok && strstr(r->text, "no answer"), "no result in 30 s: failed (%s)", r->text);
    send_line(a, "{\"v\":1,\"t\":\"result\",\"id\":9999,\"ok\":true}");
    CHECK(!a->close_why[0], "a result for nothing: ignored");
    CHECK(hl_request_inventory(&H, err, sizeof(err)) == 0 && *last_of(a, "inventory?"), "inventory asked");
    drop(a);
}

static void t_events_ring(void)
{
    fresh(NULL);
    for (int i = 0; i < HL_MAX_EVENTS + 5; i++) hl_event(&H, 6, "test", "event %d", i);
    struct buf j = { 0 };
    hl_json(&H, &j);
    CHECK(strstr(j.s, "event 44") && !strstr(j.s, "\"event 4\""), "the last %d kept", HL_MAX_EVENTS);
    buf_free(&j);
}

int main(void)
{
    snprintf(D, sizeof(D), "%s/hostlink-test-%d", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", getpid());
    hl_quiet = getenv("VERBOSE") == NULL;
    setvbuf(stdout, NULL, _IONBF, 0);
    hl_log_sink = sink;
    struct { const char *name; void (*fn)(void); } tests[] = {
        { "json", t_json }, { "config", t_config }, { "hmac", t_hmac }, { "hello", t_hello },
        { "ipv4_refused", t_ipv4_refused }, { "auth", t_auth }, { "auth_fail", t_auth_fail },
        { "not_paired", t_not_paired }, { "replay", t_replay }, { "oversize", t_oversize },
        { "malformed", t_malformed }, { "version", t_version }, { "rate", t_rate },
        { "auth_timeout", t_auth_timeout }, { "pairing", t_pairing }, { "pairing_limits", t_pairing_limits },
        { "https_pin", t_https_pin }, { "agent_disk", t_agent_disk },
        { "inventory_metrics", t_inventory_metrics }, { "edac_events", t_edac_events },
        { "gpu_temp_events", t_gpu_temp_events }, { "lost_back", t_lost_back }, { "ping", t_ping },
        { "replace", t_replace }, { "policy", t_policy }, { "turbo", t_turbo }, { "control", t_control },
        { "events_ring", t_events_ring },
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++)
    {
        int f0 = s_fail;
        tests[i].fn();
        printf("%-18s %s\n", tests[i].name, s_fail == f0 ? "ok" : "FAILED");
    }
    char cmd[300];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", D);
    if (system(cmd)) return 2;
    printf("%d checks, %d failed\n", s_pass + s_fail, s_fail);
    return s_fail ? 1 : 0;
}
