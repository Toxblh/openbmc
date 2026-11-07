/* the BMC journal for the web UI (reimu-journal.c) */
#ifndef REIMU_JOURNAL_H
#define REIMU_JOURNAL_H

#include "rbuf.h"

/* a window of the journal as JSON: count > 0 entries after cursor, < 0 before
 * it ("" = the newest) */
void journal_json(struct buf *b, const char *cursor, int count);

#endif
