/* the PNS flash object of reimu-pnsd, see reimu-pnsflash.c */
#ifndef REIMU_PNSFLASH_H
#define REIMU_PNSFLASH_H

#include <systemd/sd-bus.h>

int pnsflash_register(sd_bus *bus);

#endif
