/* reimu-hostlink: a small strict JSON reader (RFC 8259: one value, no
 * trailing commas, no leading zeros, no control characters in strings) into
 * a flat token array, and the writer helpers.
 *
 * Copyright (c) 2026 MCST REIMU contributors. Licensed under GPLv2.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hostlink.h"

/* ------------------------------------------------------------------ buf */
void buf_add(struct buf *b, const char *s, size_t n)
{
    if (b->oom) return;
    if (b->len + n + 1 > b->cap)
    {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->len + n + 1) cap *= 2;
        char *p = realloc(b->s, cap);
        if (!p)
        {
            b->oom = 1;
            return;
        }
        b->s = p;
        b->cap = cap;
    }
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = 0;
}

void buf_printf(struct buf *b, const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof(tmp))
    {
        buf_add(b, tmp, n);
        return;
    }
    char *big = malloc(n + 1);
    if (!big)
    {
        b->oom = 1;
        return;
    }
    va_start(ap, fmt);
    vsnprintf(big, n + 1, fmt, ap);
    va_end(ap);
    buf_add(b, big, n);
    free(big);
}

void buf_json_str(struct buf *b, const char *s)
{
    buf_add(b, "\"", 1);
    const char *run = s;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
    {
        const char *esc = NULL;
        char u[8];
        switch (*p)
        {
        case '"': esc = "\\\""; break;
        case '\\': esc = "\\\\"; break;
        case '\n': esc = "\\n"; break;
        case '\r': esc = "\\r"; break;
        case '\t': esc = "\\t"; break;
        default:
            if (*p < 0x20)
            {
                snprintf(u, sizeof(u), "\\u%04x", *p);
                esc = u;
            }
        }
        if (!esc) continue;
        buf_add(b, run, (const char *)p - run);
        buf_add(b, esc, strlen(esc));
        run = (const char *)p + 1;
    }
    buf_add(b, run, strlen(run));
    buf_add(b, "\"", 1);
}

void buf_consume(struct buf *b, size_t n)
{
    if (n >= b->len)
    {
        b->len = 0;
        if (b->s) b->s[0] = 0;
        return;
    }
    memmove(b->s, b->s + n, b->len - n);
    b->len -= n;
    b->s[b->len] = 0;
}

void buf_free(struct buf *b)
{
    free(b->s);
    memset(b, 0, sizeof(*b));
}

/* ------------------------------------------------------------------ reader */
struct jp
{
    const char *s;
    size_t n, i;
    struct jtok *t;
    int nt, maxt;
};

static void ws(struct jp *p)
{
    while (p->i < p->n && (p->s[p->i] == ' ' || p->s[p->i] == '\t' || p->s[p->i] == '\n' || p->s[p->i] == '\r')) p->i++;
}

static int tok(struct jp *p, int type, int start)
{
    if (p->nt >= p->maxt) return -1;
    struct jtok *t = &p->t[p->nt];
    t->type = type;
    t->start = start;
    t->end = start;
    t->size = 0;
    t->next = 0;
    return p->nt++;
}

static int hexv(int c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static int str(struct jp *p)
{
    p->i++; /* the quote */
    int k = tok(p, J_STR, (int)p->i);
    if (k < 0) return -1;
    while (p->i < p->n)
    {
        unsigned char c = p->s[p->i];
        if (c == '"')
        {
            p->t[k].end = (int)p->i++;
            p->t[k].next = p->nt;
            return k;
        }
        if (c < 0x20) return -1;
        if (c == '\\')
        {
            if (++p->i >= p->n) return -1;
            c = p->s[p->i];
            if (c == 'u')
            {
                if (p->i + 4 >= p->n) return -1;
                for (int j = 1; j <= 4; j++)
                    if (hexv(p->s[p->i + j]) < 0) return -1;
                p->i += 4;
            }
            else if (!strchr("\"\\/bfnrt", c)) return -1;
        }
        p->i++;
    }
    return -1;
}

static int num(struct jp *p)
{
    int k = tok(p, J_NUM, (int)p->i);
    if (k < 0) return -1;
    size_t i = p->i;
    if (i < p->n && p->s[i] == '-') i++;
    if (i >= p->n) return -1;
    if (p->s[i] == '0') i++;
    else if (p->s[i] >= '1' && p->s[i] <= '9')
        while (i < p->n && p->s[i] >= '0' && p->s[i] <= '9') i++;
    else return -1;
    if (i < p->n && p->s[i] == '.')
    {
        i++;
        if (i >= p->n || p->s[i] < '0' || p->s[i] > '9') return -1;
        while (i < p->n && p->s[i] >= '0' && p->s[i] <= '9') i++;
    }
    if (i < p->n && (p->s[i] == 'e' || p->s[i] == 'E'))
    {
        i++;
        if (i < p->n && (p->s[i] == '+' || p->s[i] == '-')) i++;
        if (i >= p->n || p->s[i] < '0' || p->s[i] > '9') return -1;
        while (i < p->n && p->s[i] >= '0' && p->s[i] <= '9') i++;
    }
    /* a number must end at a delimiter (01, 1x refused) */
    if (i < p->n && !strchr(" \t\r\n,]}", p->s[i])) return -1;
    p->t[k].end = (int)i;
    p->t[k].next = p->nt;
    p->i = i;
    return k;
}

static int value(struct jp *p, int depth)
{
    if (depth > J_MAX_DEPTH) return -1;
    ws(p);
    if (p->i >= p->n) return -1;
    char c = p->s[p->i];
    if (c == '"') return str(p);
    if (c == '{' || c == '[')
    {
        int k = tok(p, c == '{' ? J_OBJ : J_ARR, (int)p->i);
        if (k < 0) return -1;
        char close = c == '{' ? '}' : ']';
        p->i++;
        ws(p);
        if (p->i < p->n && p->s[p->i] == close)
        {
            p->i++;
            p->t[k].end = (int)p->i;
            p->t[k].next = p->nt;
            return k;
        }
        for (;;)
        {
            if (c == '{')
            {
                ws(p);
                if (p->i >= p->n || p->s[p->i] != '"' || str(p) < 0) return -1;
                ws(p);
                if (p->i >= p->n || p->s[p->i] != ':') return -1;
                p->i++;
            }
            if (value(p, depth + 1) < 0) return -1;
            p->t[k].size++;
            ws(p);
            if (p->i >= p->n) return -1;
            if (p->s[p->i] == ',')
            {
                p->i++;
                continue;
            }
            if (p->s[p->i] != close) return -1;
            p->i++;
            p->t[k].end = (int)p->i;
            p->t[k].next = p->nt;
            return k;
        }
    }
    static const struct { const char *w; int type; } lit[] = { { "true", J_TRUE }, { "false", J_FALSE }, { "null", J_NULL } };
    for (int j = 0; j < 3; j++)
    {
        size_t l = strlen(lit[j].w);
        if (p->n - p->i >= l && !memcmp(p->s + p->i, lit[j].w, l))
        {
            int k = tok(p, lit[j].type, (int)p->i);
            if (k < 0) return -1;
            p->i += l;
            p->t[k].end = (int)p->i;
            p->t[k].next = p->nt;
            return k;
        }
    }
    if (c == '-' || (c >= '0' && c <= '9')) return num(p);
    return -1;
}

int json_parse(const char *s, size_t n, struct jtok *t, int maxt)
{
    struct jp p = { s, n, 0, t, 0, maxt };
    if (value(&p, 1) < 0) return -1;
    ws(&p);
    return p.i == n ? p.nt : -1;
}

int json_get(const char *s, const struct jtok *t, int obj, const char *path)
{
    int cur = obj;
    while (*path)
    {
        const char *dot = strchr(path, '.');
        size_t kl = dot ? (size_t)(dot - path) : strlen(path);
        if (cur < 0 || t[cur].type != J_OBJ) return -1;
        int i = cur + 1, found = -1;
        for (int k = 0; k < t[cur].size; k++)
        {
            /* key token i, value token i + 1 */
            int vi = i + 1;
            if ((size_t)(t[i].end - t[i].start) == kl && !memcmp(s + t[i].start, path, kl)) found = vi;
            i = t[vi].next;
            if (found >= 0) break;
        }
        if (found < 0) return -1;
        cur = found;
        path += kl + (dot ? 1 : 0);
    }
    return cur;
}

int json_item(const struct jtok *t, int arr, int idx)
{
    if (arr < 0 || t[arr].type != J_ARR || idx < 0 || idx >= t[arr].size) return -1;
    int i = arr + 1;
    while (idx--) i = t[i].next;
    return i;
}

static void put_utf8(char *out, size_t n, size_t *o, unsigned cp)
{
    char b[4];
    int l;
    if (cp < 0x80) b[0] = cp, l = 1;
    else if (cp < 0x800) b[0] = 0xc0 | cp >> 6, b[1] = 0x80 | (cp & 0x3f), l = 2;
    else if (cp < 0x10000) b[0] = 0xe0 | cp >> 12, b[1] = 0x80 | ((cp >> 6) & 0x3f), b[2] = 0x80 | (cp & 0x3f), l = 3;
    else b[0] = 0xf0 | cp >> 18, b[1] = 0x80 | ((cp >> 12) & 0x3f), b[2] = 0x80 | ((cp >> 6) & 0x3f), b[3] = 0x80 | (cp & 0x3f), l = 4;
    for (int i = 0; i < l && *o + 1 < n; i++) out[(*o)++] = b[i];
}

int json_str(const char *s, const struct jtok *t, int i, char *out, size_t n)
{
    if (i < 0 || t[i].type != J_STR || !n) return -1;
    size_t o = 0;
    for (int k = t[i].start; k < t[i].end; k++)
    {
        char c = s[k];
        if (c != '\\')
        {
            if (o + 1 < n) out[o++] = c;
            continue;
        }
        c = s[++k];
        switch (c)
        {
        case 'n': c = '\n'; break;
        case 't': c = '\t'; break;
        case 'r': c = '\r'; break;
        case 'b': c = '\b'; break;
        case 'f': c = '\f'; break;
        case 'u':
        {
            unsigned cp = 0;
            for (int j = 1; j <= 4; j++) cp = cp << 4 | hexv(s[k + j]);
            k += 4;
            if (cp >= 0xd800 && cp < 0xdc00 && k + 6 < t[i].end && s[k + 1] == '\\' && s[k + 2] == 'u')
            {
                unsigned lo = 0;
                for (int j = 3; j <= 6; j++) lo = lo << 4 | hexv(s[k + j]);
                if (lo >= 0xdc00 && lo < 0xe000)
                {
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                    k += 6;
                }
            }
            if (!cp) return -1;
            put_utf8(out, n, &o, cp);
            continue;
        }
        default: break; /* " \ / */
        }
        if (o + 1 < n) out[o++] = c;
    }
    out[o] = 0;
    return 0;
}

int json_num(const char *s, const struct jtok *t, int i, double *v)
{
    if (i < 0 || t[i].type != J_NUM) return -1;
    char b[48];
    int l = t[i].end - t[i].start;
    if (l <= 0 || l >= (int)sizeof(b)) return -1;
    memcpy(b, s + t[i].start, l);
    b[l] = 0;
    *v = strtod(b, NULL);
    return 0;
}

int json_bool(const struct jtok *t, int i, int *v)
{
    if (i < 0 || (t[i].type != J_TRUE && t[i].type != J_FALSE)) return -1;
    *v = t[i].type == J_TRUE;
    return 0;
}
