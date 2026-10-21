/* BMC journal for the web UI: the stock bmcweb is built without its
 * journal log service, so reimu-pnsd reads journald itself. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-journal.h>
#include "reimu-sensors.h"

#define MSG_MAX 4096

/* value of FIELD= of the current entry, or NULL */
static const char *field(sd_journal *j, const char *name, size_t *len)
{
    const void *d;
    size_t n, k = strlen(name);
    if (sd_journal_get_data(j, name, &d, &n) < 0 || n <= k || ((const char *)d)[k] != '=') return NULL;
    *len = n - k - 1;
    return (const char *)d + k + 1;
}

static void entry(sd_journal *j, struct buf *b, int first)
{
    uint64_t t = 0;
    size_t n;
    const char *v;
    sd_journal_get_realtime_usec(j, &t);
    buf_printf(b, "%s{\"t\":%llu", first ? "" : ",", (unsigned long long)t);
    if ((v = field(j, "PRIORITY", &n)) && n == 1 && *v >= '0' && *v <= '7') buf_printf(b, ",\"p\":%c", *v);
    if ((v = field(j, "SYSLOG_IDENTIFIER", &n)) || (v = field(j, "_COMM", &n)))
    {
        buf_str(b, ",\"id\":");
        buf_json(b, v, n);
    }
    if ((v = field(j, "_PID", &n)) && n < 12)
    {
        buf_str(b, ",\"pid\":");
        buf_json(b, v, n);
    }
    if ((v = field(j, "_TRANSPORT", &n)) && n == 6 && !memcmp(v, "kernel", 6)) buf_str(b, ",\"k\":1");
    if ((v = field(j, "MESSAGE", &n)))
    {
        buf_str(b, ",\"m\":");
        buf_json(b, v, n > MSG_MAX ? MSG_MAX : n);
    }
    buf_str(b, "}");
}

static void cursor(sd_journal *j, struct buf *b, const char *key)
{
    char *c = NULL;
    if (sd_journal_get_cursor(j, &c) < 0) return;
    buf_printf(b, ",\"%s\":", key);
    buf_json(b, c, strlen(c));
    free(c);
}

/* count < 0: up to -count entries before cur (or the newest ones when cur
 * is empty); count > 0: up to count entries after cur. Entries go oldest
 * first; "first"/"last" are the cursors to continue from, "older" says
 * whether there is anything before "first". */
void journal_json(struct buf *b, const char *cur, int count)
{
    sd_journal *j;
    int back = count < 0, n = 0, want = back ? -count : count;
    if (want > 5000) want = 5000;
    if (sd_journal_open(&j, SD_JOURNAL_LOCAL_ONLY | SD_JOURNAL_SYSTEM) < 0)
    {
        buf_str(b, "{\"error\":\"journal\",\"entries\":[]}");
        return;
    }
    sd_journal_set_data_threshold(j, MSG_MAX + 64);
    int skip = 0;
    if (cur && *cur) skip = sd_journal_seek_cursor(j, cur) >= 0;
    else if (back) sd_journal_seek_tail(j);
    else sd_journal_seek_head(j);
    if (back)
    {
        if (skip) sd_journal_previous(j); /* lands on cur itself */
        int moved = 0;
        while (moved < want && sd_journal_previous(j) > 0) moved++;
        /* the walk above stepped onto the oldest wanted entry (or the
         * head); go forward from there */
        n = moved;
        buf_str(b, "{\"entries\":[");
        int first = 1, got = 0;
        if (n > 0)
            do
            {
                entry(j, b, first);
                first = 0;
            } while (++got < n && sd_journal_next(j) > 0);
        buf_str(b, "]");
        if (got)
        {
            cursor(j, b, "last");
            for (int i = 1; i < got; i++) sd_journal_previous(j);
            cursor(j, b, "first");
            buf_printf(b, ",\"older\":%s", sd_journal_previous(j) > 0 ? "true" : "false");
        }
        buf_str(b, "}");
    }
    else
    {
        if (skip) sd_journal_next(j); /* onto cur itself */
        buf_str(b, "{\"entries\":[");
        int got = 0;
        while (got < want && sd_journal_next(j) > 0) entry(j, b, !got++);
        buf_str(b, "]");
        if (got) cursor(j, b, "last");
        buf_str(b, "}");
    }
    sd_journal_close(j);
}
