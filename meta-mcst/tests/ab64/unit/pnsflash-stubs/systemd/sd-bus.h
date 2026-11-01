/* just enough of sd-bus for pnsflash_harness.c: reimu-pnsflash.c off a BMC */
#ifndef FAKE_SD_BUS_H
#define FAKE_SD_BUS_H
#include <stddef.h>
#include <stdint.h>
typedef struct sd_bus sd_bus;
typedef struct sd_bus_message sd_bus_message;
typedef struct sd_bus_error sd_bus_error;
typedef struct { int unused; } sd_bus_vtable;
#define SD_BUS_VTABLE_START(f) { 0 }
#define SD_BUS_VTABLE_END { 0 }
#define SD_BUS_PROPERTY(n, s, g, o, f) { 0 }
#define SD_BUS_METHOD(n, i, r, h, f) { 0 }
#define SD_BUS_VTABLE_UNPRIVILEGED 0
#define SD_BUS_VTABLE_PROPERTY_EMITS_INVALIDATION 0
int sd_bus_reply_method_return(sd_bus_message *m, const char *types, ...);
int sd_bus_message_read(sd_bus_message *m, const char *types, ...);
int sd_bus_message_append(sd_bus_message *m, const char *types, ...);
sd_bus *sd_bus_message_get_bus(sd_bus_message *m);
int sd_bus_get_property_string(sd_bus *bus, const char *dest, const char *path, const char *iface, const char *member,
                               sd_bus_error *err, char **out);
int sd_bus_add_object_vtable(sd_bus *bus, void *slot, const char *path, const char *iface, const sd_bus_vtable *v, void *ud);
#endif
