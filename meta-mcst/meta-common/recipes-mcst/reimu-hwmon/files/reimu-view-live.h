/* the web UI's view, its live part (reimu-view-live.c) */
#ifndef REIMU_VIEW_LIVE_H
#define REIMU_VIEW_LIVE_H

#include <systemd/sd-bus.h>

/* the service this process is: its sensors are not asked for over D-Bus */
#define VIEW_SELF "xyz.openbmc_project.Reimu.Hwmon"

int view_live_register(sd_bus *bus);
/* for reimu-hwmon --view: the bus to ask, without the object */
void view_live_bus(sd_bus *bus);
char *view_live_doc(void);

/* reimu-hwmon.c: the document of /xyz/openbmc_project/reimu/sensors, malloc'ed */
char *sensors_doc(void);

#endif
