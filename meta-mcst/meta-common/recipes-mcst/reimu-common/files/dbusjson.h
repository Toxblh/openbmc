/* D-Bus replies as JSON (dbusjson.c), shared by reimu-pnsd and reimu-hwmon */
#ifndef DBUSJSON_H
#define DBUSJSON_H

#include <systemd/sd-bus.h>

#include "rbuf.h"

#define DJ_CALL_USEC (2 * 1000 * 1000)

/* the value at the message's current position: a dictionary with string
 * keys is an object, other arrays and structs are lists, NaN is null */
int dj_value(sd_bus_message *m, struct buf *b);
/* a method call without arguments, with a timeout: the reply or NULL */
sd_bus_message *dj_call(sd_bus *bus, const char *dest, const char *path, const char *iface, const char *member);
/* GetManagedObjects of a service at om: path -> interface -> properties, the
 * paths under prefix only, appended without the braces (for merging; first:
 * nothing before it). Returns first, cleared once something was appended */
int dj_managed(sd_bus *bus, struct buf *b, const char *dest, const char *om, const char *prefix, int first);
/* "path":{properties of iface}, {} if they can't be read */
void dj_object(sd_bus *bus, struct buf *b, const char *dest, const char *path, const char *iface);

#endif
