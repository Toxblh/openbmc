/*
 * A small growable string for the JSON documents of reimu-pnsd and
 * reimu-hwmon (rbuf.h).
 *
 * Copyright (c) 2026 MCST REIMU contributors. Licensed under GPLv2.
 */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rbuf.h"

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
