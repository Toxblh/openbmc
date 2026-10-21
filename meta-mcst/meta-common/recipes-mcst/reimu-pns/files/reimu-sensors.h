/* shared by reimu-pnsd.c, reimu-sensors.c and reimu-journal.c */
#ifndef REIMU_SENSORS_H
#define REIMU_SENSORS_H

#include <stddef.h>

/* small growable string */
struct buf
{
    char *s;
    size_t len, cap;
};

void buf_add(struct buf *b, const char *s, size_t n);
void buf_str(struct buf *b, const char *s);
void buf_printf(struct buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void buf_json(struct buf *b, const char *s, size_t n);

/* appends "sensors":[...] (and "error" when there is no device tree) */
void sensors_json(struct buf *b);

/* BMC journal window as JSON, see reimu-journal.c */
void journal_json(struct buf *b, const char *cursor, int count);

#endif
