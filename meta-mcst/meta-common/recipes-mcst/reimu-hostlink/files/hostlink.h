/* reimu-hostlink: the BMC's end of the link with the host agent (reimu-hostd).
 *
 *   hl-json.c   a small JSON reader (tokens, paths) and writer helpers
 *   hl-proto.c  the protocol and the state: hello/auth/pair, limits, the
 *               agent's inventory and metrics, sensors, events, the GPU fan
 *               policy, control requests; no I/O but the state files
 *   hl-net.c    TLS (OpenSSL), the listening sockets, the discovery answer
 *   hl-dbus.c   D-Bus: the service, the sensors, Logging (not in the tests)
 *   reimu-hostlink.c  the daemon: config, poll loop, signals, watchdog
 *
 * Copyright (c) 2026 MCST REIMU contributors. Licensed under GPLv2.
 */
#ifndef HOSTLINK_H
#define HOSTLINK_H

#include <stddef.h>
#include <stdint.h>
#include <poll.h>

#define HL_VERSION   "1.0"
#define HL_PROTO     1
#define HL_PORT      5564
#define HL_MAX_LINE  (128 * 1024) /* after auth (the inventory) */
#define HL_PRE_LINE  2048         /* before auth */
#define HL_RATE_BURST 40          /* messages, refilled HL_RATE_PER_S a second */
#define HL_RATE_PER_S 4
#define HL_MAX_EVENTS 40
#define HL_MAX_RESULTS 8
#define HL_MAX_DIMMS  32
#define HL_KEY_LEN    32

/* ------------------------------------------------------------------ buffers */
struct buf
{
    char *s;
    size_t len, cap;
    int oom;
};
void buf_printf(struct buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void buf_add(struct buf *b, const char *s, size_t n);
void buf_json_str(struct buf *b, const char *s); /* "..." escaped */
void buf_consume(struct buf *b, size_t n);       /* drop the first n bytes */
void buf_free(struct buf *b);

/* ------------------------------------------------------------------ JSON */
enum { J_OBJ = 1, J_ARR, J_STR, J_NUM, J_TRUE, J_FALSE, J_NULL };
struct jtok
{
    int type;
    int start, end; /* bytes of the value (a string: without the quotes) */
    int size;       /* object: keys, array: items */
    int next;       /* index of the token after this value and its children */
};
#define J_MAX_DEPTH 16
/* the whole text one JSON value (whitespace around allowed): the number of
 * tokens, or -1 (malformed, too deep, more than maxt tokens) */
int json_parse(const char *s, size_t n, struct jtok *t, int maxt);
/* the value of a key path "a.b.c" under object token obj, -1 = none */
int json_get(const char *s, const struct jtok *t, int obj, const char *path);
/* the i-th item of array token arr, -1 = none */
int json_item(const struct jtok *t, int arr, int i);
/* string (unescaped, \uXXXX as UTF-8; NUL refused), 0 ok / -1 */
int json_str(const char *s, const struct jtok *t, int i, char *out, size_t n);
int json_num(const char *s, const struct jtok *t, int i, double *v);
int json_bool(const struct jtok *t, int i, int *v);

/* ------------------------------------------------------------------ config */
struct hl_config
{
    int port;
    char iface[16];       /* SO_BINDTODEVICE; "" = any */
    int allow_ipv4;       /* IPv4 (v4-mapped) peers accepted */
    int ping_ms;          /* to the agent */
    int lost_ms;          /* no agent this long: "lost" */
    int auth_timeout_ms;
    int control_timeout_ms;
    int pair_ttl_ms;
    double warn[3], crit[3]; /* GPU edge, junction, memory, °C */
    int follow_fand_turbo;
    int logging;          /* warning/critical events as Logging entries */
    char https_cert[128]; /* bmcweb's: the pin of its key for the install command */
    char disk_helper[128]; /* reimu-agent-disk: the installer's USB disk */
};
void hl_config_defaults(struct hl_config *c);
/* KEY=VALUE text over c: the number of errors (in err[], defaults kept) */
int hl_config_parse(struct hl_config *c, const char *text, char err[][128], int maxerr);

/* ------------------------------------------------------------------ sensors */
enum { S_GPU_EDGE, S_GPU_JUNCTION, S_GPU_MEM, S_GPU_FAN, S_GPU_PWM, S_GPU_POWER, S_DISK_MAX, NSENS };
struct hl_sensor_def
{
    const char *name, *type, *unit, *path;
    double min, max;
    int temp; /* index into warn/crit, -1 none */
};
extern const struct hl_sensor_def hl_sensors[NSENS];

/* ------------------------------------------------------------------ state */
enum { C_HELLO, C_AUTHED, C_CLOSING };
enum { T_IPV4, T_IPV6_LL, T_IPV6 };

struct hl_conn
{
    int state;
    int id;              /* for the logs */
    char nonce[33];
    char peer[64];
    int transport;
    struct buf in, out;
    int64_t opened, last_rx;
    double tokens;       /* rate limit bucket */
    int64_t tokens_at;
    int malformed;
    char close_why[96];  /* set: the net layer closes after sending out */
};

struct hl_event
{
    int64_t wall;  /* s since the epoch */
    int prio;      /* syslog: 2 crit, 4 warning, 5 notice, 6 info */
    char kind[24];
    char text[160];
};

struct hl_result
{
    int id;
    char op[16];
    int done, ok;
    int64_t sent;
    char text[160];
};

struct hl
{
    struct hl_config c;
    char dir[160];        /* persistent state (key, policy, edac); "" = none */
    unsigned char key[HL_KEY_LEN];
    int paired;
    char token[33];       /* pairing, one use */
    int64_t token_until;
    int token_tries;
    char fingerprint[65]; /* of the TLS certificate (net layer) */
    char mac[18], ipv4[16]; /* of IFACE, for the pairing string */

    struct hl_conn *agent; /* the authenticated one */
    char agent_id[65], agent_host[65], agent_ver[17];
    int agent_transport;
    char agent_peer[64];
    int64_t agent_since_wall, last_seen_wall, last_seen, gone_at;
    int ever, lost_reported;
    int64_t last_ping;

    struct buf inventory, metrics; /* raw JSON of the agent's last ones */
    int64_t inventory_at, metrics_at;
    double sens[NSENS];   /* NAN = none */
    int alarm[3];         /* GPU temps: 0, 1 warning, 2 critical */
    long long ce, ue;     /* last seen (and persisted) */
    int have_edac;
    struct { char label[32]; long long ce, ue; } dimm[HL_MAX_DIMMS];
    int ndimm;

    char fan_mode[8];     /* the GPU fan policy: auto / manual */
    double fan_duty;
    int64_t turbo_until;  /* monotonic ms, 0 = none */
    int fand_turbo;       /* turbo set because fand's is on */

    struct hl_event ev[HL_MAX_EVENTS];
    int nev, evpos;
    struct hl_result res[HL_MAX_RESULTS];
    int nres, respos, next_id;

    unsigned gen;         /* bumped on every change D-Bus shows */
    unsigned sens_gen;    /* bumped when a sensor value changes */
    int64_t auth_fail_logged;
    int auth_fails;
    /* the event sink (journal + Logging); default: stderr */
    void (*on_event)(struct hl *h, const struct hl_event *e);
    int64_t now;          /* the last hl_tick/input time, ms monotonic */

    /* the installer's virtual USB disk (reimu-agent-disk, by the daemon) */
    int disk_on;
    int64_t disk_since, disk_until; /* monotonic ms */
    int64_t paired_at, authed_at;   /* the last pairing, the last authentication */
    char pair_str[600];   /* the pairing string on the disk: wiped when it goes */
};

/* dir: persistent files (key, policy, edac), NULL = in memory only */
void hl_init(struct hl *h, const struct hl_config *c, const char *dir);
void hl_conn_open(struct hl *h, struct hl_conn *conn, const char *peer, int64_t now);
void hl_conn_input(struct hl *h, struct hl_conn *conn, const char *data, size_t n, int64_t now);
/* every second for each connection: its timeouts */
void hl_conn_tick(struct hl *h, struct hl_conn *conn, int64_t now);
/* the connection is gone (by the net layer) */
void hl_conn_closed(struct hl *h, struct hl_conn *conn, int64_t now);
void hl_conn_free(struct hl_conn *conn);
void hl_tick(struct hl *h, int64_t now);

/* requests (D-Bus): 0 ok, -1 with a message in err */
int hl_set_gpu_fan(struct hl *h, const char *mode, double duty, char *err, size_t n);
int hl_gpu_turbo(struct hl *h, int minutes, char *err, size_t n); /* 0 = off */
int hl_os_power(struct hl *h, const char *action, char *err, size_t n);
int hl_edac_load(struct hl *h, char *err, size_t n);
int hl_request_inventory(struct hl *h, char *err, size_t n);
/* fand's turbo: on (seconds left) or off (0) */
void hl_fand_turbo(struct hl *h, int seconds_left);
/* a new pairing token; out: JSON {token, pair (the string for the agent),
 * command, expires} */
int hl_pair_begin(struct hl *h, struct buf *out, char *err, size_t n);
void hl_unpair(struct hl *h);
/* the installer's USB disk: a new pairing (as hl_pair_begin, plus "disk":true;
 * h->pair_str for the disk's PAIRING.TXT); due: the agent paired and connected
 * since, or the token's time is up (the daemon then takes the disk away and
 * calls hl_disk_end) */
int hl_disk_begin(struct hl *h, struct buf *out, char *err, size_t n);
int hl_disk_due(const struct hl *h);
void hl_disk_end(struct hl *h);
/* the daemon's (reimu-hostlink.c): the disk made by DISK_HELPER start (the
 * pairing string on its stdin; its error message back), taken away by
 * DISK_HELPER stop; 0 ok, -1 with err */
int daemon_disk_start(struct hl *h, struct buf *out, char *err, size_t n);
int daemon_disk_stop(struct hl *h, const char *why, char *err, size_t n);

void hl_json(struct hl *h, struct buf *b);
/* "ok", "late", "lost", "never" */
const char *hl_heartbeat(const struct hl *h);
int hl_connected(const struct hl *h);
void hl_event(struct hl *h, int prio, const char *kind, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
/* the GPU temperature for reimu-fand: junction (edge if none), °C, NAN */
double hl_gpu_temp(const struct hl *h);

/* random bytes (getrandom), 0 ok */
int hl_random(void *p, size_t n);
void hl_hex(const unsigned char *p, size_t n, char *out);
/* HMAC-SHA256 as lowercase hex (65 bytes) */
void hl_hmac_hex(const unsigned char *key, size_t klen, const char *msg, char *out);

extern int hl_quiet;
/* every log line also goes here when set (tests: no secret in the logs) */
extern void (*hl_log_sink)(int prio, const char *line);
void hl_log(int prio, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* ------------------------------------------------------------------ net (hl-net.c) */
struct hl_net;
/* the certificate (made on first start), the sockets; NULL + err on failure */
struct hl_net *net_new(struct hl *h, const char *certfile, char *err, size_t n);
/* fills pollfds (at most max), returns how many */
int net_pollfds(struct hl_net *nt, struct pollfd *p, int max);
void net_handle(struct hl_net *nt, struct pollfd *p, int n, int64_t now);
/* the listening sockets again (the interface came, IFACE changed) */
void net_retry(struct hl_net *nt, int64_t now);
int net_listening(struct hl_net *nt);
void net_free(struct hl_net *nt);
/* SHA-256 of a PEM certificate's DER as hex, 0 ok */
int net_fingerprint(const char *certfile, char out[65]);

#endif
