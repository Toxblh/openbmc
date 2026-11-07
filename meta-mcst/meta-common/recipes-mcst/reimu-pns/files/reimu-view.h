#ifndef REIMU_VIEW_H
#define REIMU_VIEW_H

#include <systemd/sd-bus.h>

/* the web UI's view, its static part: /xyz/openbmc_project/reimu/view/static (reimu-view.c) */
int view_register(sd_bus *bus);
int view_print(sd_bus *bus, const char *which);

/* the ПНС document, malloc'ed (reimu-pnsd.c) */
char *pns_doc(void);

#endif
