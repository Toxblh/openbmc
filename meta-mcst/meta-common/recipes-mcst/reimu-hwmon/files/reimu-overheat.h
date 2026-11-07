/* reimu-overheat.c: the overheat protection's D-Bus object of reimu-hwmon */
#ifndef REIMU_OVERHEAT_H
#define REIMU_OVERHEAT_H

#include <systemd/sd-bus.h>

int overheat_register(sd_bus *bus);
/* its document (and the web UI's view's), malloc'ed */
char *overheat_doc(void);

#endif
