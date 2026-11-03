#ifndef REIMU_VIEW_H
#define REIMU_VIEW_H

#include <systemd/sd-bus.h>

/* the web UI's view: /xyz/openbmc_project/reimu/view/{live,static} (reimu-view.c) */
int view_register(sd_bus *bus);
int view_print(sd_bus *bus, const char *which);

/* the documents of reimu-pnsd's own objects, malloc'ed (reimu-pnsd.c, reimu-overheat.c) */
char *pns_doc(void);
char *sensors_doc(int force);
char *overheat_doc(void);

#endif
