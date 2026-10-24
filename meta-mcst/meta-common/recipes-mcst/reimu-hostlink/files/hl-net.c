/* reimu-hostlink: TLS (OpenSSL, the image has it for bmcweb) on TCP [::]:PORT
 * (dual stack: IPv6 link-local first, IPv4 too unless ALLOW_IPV4=no), bound
 * to IFACE (SO_BINDTODEVICE: only the board's LAN, eth0; eth1 never), and
 * the discovery answer on UDP [::]:PORT to link-local peers only.
 *
 * The certificate is the daemon's own (EC P-256, self-signed, made on the
 * first start in the state directory): the agent pins its SHA-256 at
 * pairing, and it doesn't change when bmcweb's HTTPS certificate does.
 *
 * Copyright (c) 2026 MCST REIMU contributors. Licensed under GPLv2.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "hostlink.h"

#define MAX_CLIENTS 6  /* the agent, a new one replacing it, a few strangers */
#define MAX_PENDING 4  /* not authenticated yet */
#define RETRY_MS 10000

struct client
{
    int fd;
    SSL *ssl;
    int hs_done, shut;
    struct hl_conn c;
};

struct hl_net
{
    struct hl *h;
    SSL_CTX *ctx;
    int lfd, ufd;
    char iface[16];
    int port;
    int64_t last_retry, udp_window;
    int udp_count;
    int logged_bind;
    struct client cl[MAX_CLIENTS];
};

/* ------------------------------------------------------------------ certificate */
int net_fingerprint(const char *certfile, char out[65])
{
    FILE *f = fopen(certfile, "r");
    if (!f) return -1;
    X509 *x = PEM_read_X509(f, NULL, NULL, NULL);
    fclose(f);
    if (!x) return -1;
    unsigned char md[32];
    unsigned int n = 0;
    int r = X509_digest(x, EVP_sha256(), md, &n);
    X509_free(x);
    if (r != 1 || n != 32) return -1;
    hl_hex(md, 32, out);
    return 0;
}

/* key and certificate into one PEM file, root only */
static int make_cert(const char *file, char *err, size_t n)
{
    int ok = 0;
    EVP_PKEY *pk = NULL;
    X509 *x = NULL;
    EVP_PKEY_CTX *kc = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, NULL);
    if (!kc || EVP_PKEY_keygen_init(kc) != 1 || EVP_PKEY_CTX_set_ec_paramgen_curve_nid(kc, NID_X9_62_prime256v1) != 1 ||
        EVP_PKEY_keygen(kc, &pk) != 1)
        goto out;
    x = X509_new();
    if (!x) goto out;
    X509_set_version(x, 2);
    unsigned char sn[8];
    if (hl_random(sn, sizeof(sn))) goto out;
    sn[0] &= 0x7f;
    BIGNUM *bn = BN_bin2bn(sn, sizeof(sn), NULL);
    BN_to_ASN1_INTEGER(bn, X509_get_serialNumber(x));
    BN_free(bn);
    X509_gmtime_adj(X509_getm_notBefore(x), -86400);
    X509_gmtime_adj(X509_getm_notAfter(x), 30L * 365 * 86400); /* pinned, not checked for dates */
    X509_set_pubkey(x, pk);
    X509_NAME *name = X509_get_subject_name(x);
    X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC, (const unsigned char *)"REIMU BMC", -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char *)"reimu-hostlink", -1, -1, 0);
    X509_set_issuer_name(x, name);
    if (!X509_sign(x, pk, EVP_sha256())) goto out;
    char tmp[300];
    snprintf(tmp, sizeof(tmp), "%s.new", file);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) goto out;
    FILE *f = fdopen(fd, "w");
    if (!f)
    {
        close(fd);
        goto out;
    }
    ok = PEM_write_PrivateKey(f, pk, NULL, NULL, 0, NULL, NULL) == 1 && PEM_write_X509(f, x) == 1;
    ok = fflush(f) == 0 && fsync(fileno(f)) == 0 && ok;
    fclose(f);
    if (!ok || rename(tmp, file))
    {
        unlink(tmp);
        ok = 0;
    }
out:
    if (!ok) snprintf(err, n, "certificate %s not made: %s", file, ERR_error_string(ERR_get_error(), NULL));
    X509_free(x);
    EVP_PKEY_free(pk);
    EVP_PKEY_CTX_free(kc);
    return ok ? 0 : -1;
}

/* ------------------------------------------------------------------ sockets */
static void iface_info(struct hl_net *nt)
{
    struct hl *h = nt->h;
    if (!nt->iface[0]) return;
    char p[80], mac[32] = "";
    snprintf(p, sizeof(p), "/sys/class/net/%s/address", nt->iface);
    FILE *f = fopen(p, "r");
    if (f)
    {
        if (fgets(mac, sizeof(mac), f)) mac[strcspn(mac, "\n")] = 0;
        fclose(f);
    }
    snprintf(h->mac, sizeof(h->mac), "%s", mac);
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", nt->iface);
    h->ipv4[0] = 0;
    if (s >= 0 && ioctl(s, SIOCGIFADDR, &ifr) == 0)
        inet_ntop(AF_INET, &((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr, h->ipv4, sizeof(h->ipv4));
    if (s >= 0) close(s);
}

static int open_sock(struct hl_net *nt, int type, char *err, size_t n)
{
    int fd = socket(AF_INET6, type | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
    {
        snprintf(err, n, "socket: %s", strerror(errno));
        return -1;
    }
    int off = 0, on = 1;
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    if (nt->iface[0] && setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, nt->iface, strlen(nt->iface) + 1))
    {
        snprintf(err, n, "%s: %s", nt->iface, strerror(errno));
        close(fd);
        return -1;
    }
    struct sockaddr_in6 a;
    memset(&a, 0, sizeof(a));
    a.sin6_family = AF_INET6;
    a.sin6_port = htons(nt->port);
    a.sin6_addr = in6addr_any;
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) || (type == SOCK_STREAM && listen(fd, 8)))
    {
        snprintf(err, n, "[::]:%d: %s", nt->port, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

void net_retry(struct hl_net *nt, int64_t now)
{
    nt->last_retry = now;
    iface_info(nt);
    char err[160] = "";
    if (nt->lfd < 0) nt->lfd = open_sock(nt, SOCK_STREAM, err, sizeof(err));
    if (nt->ufd < 0 && nt->lfd >= 0) nt->ufd = open_sock(nt, SOCK_DGRAM, err, sizeof(err));
    if (nt->lfd < 0 && !nt->logged_bind)
    {
        hl_log(4, "not listening yet (%s): trying every %d s", err, RETRY_MS / 1000);
        nt->logged_bind = 1;
    }
    else if (nt->lfd >= 0 && nt->logged_bind != 2)
    {
        hl_log(5, "listening on TCP+UDP [::]:%d%s%s (TLS, certificate %.16s…)", nt->port, nt->iface[0] ? " on " : "",
               nt->iface, nt->h->fingerprint);
        nt->logged_bind = 2;
    }
}

int net_listening(struct hl_net *nt)
{
    return nt->lfd >= 0;
}

struct hl_net *net_new(struct hl *h, const char *certfile, char *err, size_t n)
{
    struct stat st;
    if (stat(certfile, &st) != 0 && make_cert(certfile, err, n)) return NULL;
    if (net_fingerprint(certfile, h->fingerprint))
    {
        snprintf(err, n, "%s: not a certificate", certfile);
        return NULL;
    }
    struct hl_net *nt = calloc(1, sizeof(*nt));
    if (!nt) return NULL;
    nt->h = h;
    nt->lfd = nt->ufd = -1;
    nt->port = h->c.port;
    snprintf(nt->iface, sizeof(nt->iface), "%s", h->c.iface);
    for (int i = 0; i < MAX_CLIENTS; i++) nt->cl[i].fd = -1;
    nt->ctx = SSL_CTX_new(TLS_server_method());
    if (!nt->ctx) goto fail;
    SSL_CTX_set_min_proto_version(nt->ctx, TLS1_2_VERSION);
    SSL_CTX_set_options(nt->ctx, SSL_OP_CIPHER_SERVER_PREFERENCE | SSL_OP_NO_RENEGOTIATION | SSL_OP_NO_TICKET);
    SSL_CTX_set_ciphersuites(nt->ctx, "TLS_CHACHA20_POLY1305_SHA256:TLS_AES_128_GCM_SHA256:TLS_AES_256_GCM_SHA384");
    SSL_CTX_set_cipher_list(nt->ctx, "ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-ECDSA-AES256-GCM-SHA384");
    if (SSL_CTX_use_certificate_chain_file(nt->ctx, certfile) != 1 ||
        SSL_CTX_use_PrivateKey_file(nt->ctx, certfile, SSL_FILETYPE_PEM) != 1)
        goto fail;
    net_retry(nt, 0);
    return nt;
fail:
    snprintf(err, n, "TLS: %s", ERR_error_string(ERR_get_error(), NULL));
    if (nt->ctx) SSL_CTX_free(nt->ctx);
    free(nt);
    return NULL;
}

/* ------------------------------------------------------------------ clients */
static void client_close(struct hl_net *nt, struct client *cl, int64_t now)
{
    if (cl->ssl)
    {
        if (cl->hs_done) SSL_shutdown(cl->ssl);
        SSL_free(cl->ssl);
    }
    close(cl->fd);
    if (cl->c.close_why[0] && strcmp(cl->c.close_why, "paired"))
        hl_log(6, "connection %d (%s) closed: %s", cl->c.id, cl->c.peer, cl->c.close_why);
    hl_conn_closed(nt->h, &cl->c, now);
    hl_conn_free(&cl->c);
    memset(cl, 0, sizeof(*cl));
    cl->fd = -1;
}

static void client_write(struct client *cl)
{
    while (cl->hs_done && cl->c.out.len)
    {
        int n = SSL_write(cl->ssl, cl->c.out.s, (int)(cl->c.out.len > 65536 ? 65536 : cl->c.out.len));
        if (n <= 0)
        {
            int e = SSL_get_error(cl->ssl, n);
            if (e != SSL_ERROR_WANT_WRITE && e != SSL_ERROR_WANT_READ && !cl->c.close_why[0])
                snprintf(cl->c.close_why, sizeof(cl->c.close_why), "write failed");
            return;
        }
        buf_consume(&cl->c.out, n);
    }
}

static void client_io(struct hl_net *nt, struct client *cl, int64_t now)
{
    if (!cl->hs_done)
    {
        int r = SSL_accept(cl->ssl);
        if (r != 1)
        {
            int e = SSL_get_error(cl->ssl, r);
            if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) return;
            snprintf(cl->c.close_why, sizeof(cl->c.close_why), "TLS handshake failed");
            ERR_clear_error();
            return;
        }
        cl->hs_done = 1;
    }
    char b[16384];
    for (int rounds = 0; rounds < 16 && !cl->c.close_why[0]; rounds++)
    {
        int n = SSL_read(cl->ssl, b, sizeof(b));
        if (n <= 0)
        {
            int e = SSL_get_error(cl->ssl, n);
            if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) break;
            snprintf(cl->c.close_why, sizeof(cl->c.close_why), e == SSL_ERROR_ZERO_RETURN ? "closed by the peer" : "connection lost");
            ERR_clear_error();
            break;
        }
        hl_conn_input(nt->h, &cl->c, b, n, now);
    }
    client_write(cl);
}

static void peer_name(const struct sockaddr_in6 *a, char *out, size_t n)
{
    char ip[INET6_ADDRSTRLEN] = "?", ifn[IF_NAMESIZE] = "";
    inet_ntop(AF_INET6, &a->sin6_addr, ip, sizeof(ip));
    if (IN6_IS_ADDR_LINKLOCAL(&a->sin6_addr) && a->sin6_scope_id && if_indextoname(a->sin6_scope_id, ifn))
        snprintf(out, n, "%s%%%s", ip, ifn);
    else snprintf(out, n, "%s", ip);
}

static void on_accept(struct hl_net *nt, int64_t now)
{
    for (int k = 0; k < 8; k++)
    {
        struct sockaddr_in6 a;
        socklen_t al = sizeof(a);
        int fd = accept4(nt->lfd, (struct sockaddr *)&a, &al, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) return;
        int pending = 0;
        struct client *free_cl = NULL;
        for (int i = 0; i < MAX_CLIENTS; i++)
        {
            if (nt->cl[i].fd < 0)
            {
                if (!free_cl) free_cl = &nt->cl[i];
            }
            else if (nt->cl[i].c.state == C_HELLO) pending++;
        }
        if (!free_cl || pending >= MAX_PENDING)
        {
            close(fd); /* busy: the agent tries again */
            continue;
        }
        int on = 1, idle = 30, intvl = 10, cnt = 3;
        setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
        setsockopt(fd, IPPROTO_TCP, 4 /* TCP_KEEPIDLE */, &idle, sizeof(idle));
        setsockopt(fd, IPPROTO_TCP, 5 /* TCP_KEEPINTVL */, &intvl, sizeof(intvl));
        setsockopt(fd, IPPROTO_TCP, 6 /* TCP_KEEPCNT */, &cnt, sizeof(cnt));
        struct client *cl = free_cl;
        memset(cl, 0, sizeof(*cl));
        cl->fd = fd;
        cl->ssl = SSL_new(nt->ctx);
        if (!cl->ssl)
        {
            close(fd);
            cl->fd = -1;
            continue;
        }
        SSL_set_fd(cl->ssl, fd);
        SSL_set_mode(cl->ssl, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
        char peer[64];
        peer_name(&a, peer, sizeof(peer));
        hl_conn_open(nt->h, &cl->c, peer, now);
        hl_log(7, "connection %d from %s", cl->c.id, peer);
        if (!cl->c.close_why[0]) client_io(nt, cl, now);
    }
}

/* the discovery answer: "REIMU-HOSTLINK? 1" from a link-local peer ->
 * "REIMU-HOSTLINK 1 PORT FINGERPRINT-PREFIX" (rate limited: no amplifier) */
static void on_udp(struct hl_net *nt, int64_t now)
{
    for (int k = 0; k < 16; k++)
    {
        char b[64];
        struct sockaddr_in6 a;
        socklen_t al = sizeof(a);
        ssize_t n = recvfrom(nt->ufd, b, sizeof(b) - 1, 0, (struct sockaddr *)&a, &al);
        if (n < 0) return;
        b[n] = 0;
        while (n && (b[n - 1] == '\n' || b[n - 1] == '\r' || b[n - 1] == ' ')) b[--n] = 0;
        if (strcmp(b, "REIMU-HOSTLINK? 1") || !IN6_IS_ADDR_LINKLOCAL(&a.sin6_addr)) continue;
        if (now - nt->udp_window >= 1000) nt->udp_window = now, nt->udp_count = 0;
        if (++nt->udp_count > 10) continue;
        char r[96];
        int l = snprintf(r, sizeof(r), "REIMU-HOSTLINK 1 %d %.16s", nt->port, nt->h->fingerprint);
        sendto(nt->ufd, r, l, 0, (struct sockaddr *)&a, al);
    }
}

int net_pollfds(struct hl_net *nt, struct pollfd *p, int max)
{
    int n = 0;
    if (nt->lfd >= 0 && n < max) p[n++] = (struct pollfd){ nt->lfd, POLLIN, 0 };
    if (nt->ufd >= 0 && n < max) p[n++] = (struct pollfd){ nt->ufd, POLLIN, 0 };
    for (int i = 0; i < MAX_CLIENTS && n < max; i++)
    {
        struct client *cl = &nt->cl[i];
        if (cl->fd < 0) continue;
        short ev = POLLIN;
        if (!cl->hs_done || cl->c.out.len) ev |= POLLOUT;
        p[n++] = (struct pollfd){ cl->fd, ev, 0 };
    }
    return n;
}

void net_handle(struct hl_net *nt, struct pollfd *p, int n, int64_t now)
{
    for (int k = 0; k < n; k++)
    {
        if (!p[k].revents) continue;
        if (p[k].fd == nt->lfd) on_accept(nt, now);
        else if (p[k].fd == nt->ufd) on_udp(nt, now);
        else
            for (int i = 0; i < MAX_CLIENTS; i++)
                if (nt->cl[i].fd == p[k].fd)
                {
                    client_io(nt, &nt->cl[i], now);
                    if (p[k].revents & (POLLHUP | POLLERR) && !(p[k].revents & POLLIN) && !nt->cl[i].c.close_why[0])
                        snprintf(nt->cl[i].c.close_why, sizeof(nt->cl[i].c.close_why), "connection lost");
                }
    }
    /* what the state queued since (pings, policy, control), the timeouts,
     * and the connections to close (after their last words) */
    static int64_t last_tick;
    int tick = now - last_tick >= 1000;
    if (tick) last_tick = now;
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        struct client *cl = &nt->cl[i];
        if (cl->fd < 0) continue;
        if (tick) hl_conn_tick(nt->h, &cl->c, now);
        client_write(cl);
        if (cl->c.close_why[0] && (!cl->c.out.len || !cl->hs_done || cl->shut++ > 20)) client_close(nt, cl, now);
    }
    if (nt->lfd < 0 && now - nt->last_retry >= RETRY_MS) net_retry(nt, now);
    else if (now - nt->last_retry >= 60000) net_retry(nt, now); /* the IPv4 address for Pair() */
}

void net_free(struct hl_net *nt)
{
    if (!nt) return;
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (nt->cl[i].fd >= 0) client_close(nt, &nt->cl[i], 0);
    if (nt->lfd >= 0) close(nt->lfd);
    if (nt->ufd >= 0) close(nt->ufd);
    SSL_CTX_free(nt->ctx);
    free(nt);
}
