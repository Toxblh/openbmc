/* reimu-hostlink: the D-Bus side (hl-dbus.c); without HL_DBUS (the native
 * tests) none of it.
 *
 * Copyright (c) 2026 MCST REIMU contributors. Licensed under GPLv2.
 */
#ifndef HL_DBUS_H
#define HL_DBUS_H

#include <poll.h>
#include "hostlink.h"

#ifdef HL_DBUS
int dbus_init(struct hl *h);
int dbus_pollfd(struct pollfd *p); /* 0 or 1 */
int dbus_timeout_ms(void);         /* -1 none */
void dbus_process(void);
void dbus_tick(struct hl *h, int64_t now); /* every second: fand's turbo */
void dbus_publish(struct hl *h);   /* PropertiesChanged of what changed */
void dbus_notify(const char *state);
void dbus_close(void);
#else
static inline int dbus_init(struct hl *h) { return 0; }
static inline int dbus_pollfd(struct pollfd *p) { return 0; }
static inline int dbus_timeout_ms(void) { return -1; }
static inline void dbus_process(void) {}
static inline void dbus_tick(struct hl *h, int64_t now) {}
static inline void dbus_publish(struct hl *h) {}
static inline void dbus_notify(const char *state) {}
static inline void dbus_close(void) {}
#endif

#endif
