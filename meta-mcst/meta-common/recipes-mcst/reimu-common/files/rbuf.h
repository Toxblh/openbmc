/* a small growable string (rbuf.c), shared by reimu-pnsd and reimu-hwmon */
#ifndef RBUF_H
#define RBUF_H

#include <stddef.h>

struct buf
{
    char *s;
    size_t len, cap;
};

void buf_add(struct buf *b, const char *s, size_t n);
void buf_str(struct buf *b, const char *s);
/* (for short pieces: it cuts at 511 bytes) */
void buf_printf(struct buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* a JSON string literal; invalid UTF-8 bytes are replaced with U+FFFD */
void buf_json(struct buf *b, const char *s, size_t n);

#endif
