/* reimu-overheat.c: the overheat protection's D-Bus object of reimu-pnsd */
#ifndef REIMU_OVERHEAT_H
#define REIMU_OVERHEAT_H

#include <systemd/sd-bus.h>

int overheat_register(sd_bus *bus);

#endif
