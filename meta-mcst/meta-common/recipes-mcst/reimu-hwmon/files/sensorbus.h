/* reimu-hwmon's sensors on D-Bus (sensorbus.c) */
#ifndef SENSORBUS_H
#define SENSORBUS_H

#include <systemd/sd-bus.h>

#include "hwmon.h"
#include "rbuf.h"

#define SB_CHASSIS "/xyz/openbmc_project/inventory/system/chassis/motherboard"

void sb_init(sd_bus *bus);
/* the sensors now (n = 0: none): each one with a D-Bus name and a value is
 * an object, added, changed or removed as it comes, goes or changes */
void sb_update(const struct entry *e, int n);
/* the objects as GetManagedObjects gives them, path -> interface ->
 * properties, appended without the braces; returns first as dj_managed() */
int sb_json(struct buf *b, int first);
int sb_count(void);

#endif
