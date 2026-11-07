/* reimu-hwmon: the platform's sensors, read once and given to everyone
 * (D-Bus Sensor.Value for Redfish and IPMI, the web UI's documents,
 * Prometheus). Shared by its parts and the native tests (tests/).
 *
 * Copyright (c) 2026 MCST REIMU contributors. Licensed under GPLv2.
 */
#ifndef HWMON_H
#define HWMON_H

#include <stddef.h>
#include <stdint.h>

#include "rbuf.h"

/* ---- the platform's devices, from the ПНС device tree (hwmon-dt.c) */
enum { DK_LM96163, DK_EMC2305, DK_PMBUS, DK_IR3806X, DK_JC42, DK_PCS };
#define MAX_DEVS 48
struct device
{
    int kind, bus, addr; /* PCS: its bus, the address of its first sensor */
    int cpu;             /* PCS: the processor */
    char label[96];      /* the node's label, else its name */
    char tlabel[2][48];  /* lm96163: temp1_label, temp2_label ("" none) */
    int fan_base;        /* EMC2305: the board's number of its first fan (label TACH[n..]) */
    int64_t retry_at;    /* one of ours that didn't answer: not tried again before (ms) */
};
struct catalog
{
    int n;
    struct device d[MAX_DEVS];
};
/* the devices whose sensors are known, in the device tree's order, then the
 * PCS nodes (pcs@N: processor N's sensors at 0x20 + 0x10 * N on pcs_bus).
 * 0, -1: no (valid) device tree */
int catalog_load(struct catalog *c, const char *dtb, int pcs_bus);

/* ---- readings: reimu-fand's (its Readings over D-Bus: the chips it drives,
 * the inputs of its control) and this daemon's own */
struct reading
{
    char dev[12];  /* "6-004c" */
    char input[8]; /* hwmon's names: temp1, temp2, in1, fan1.. */
    double value;  /* NAN: none */
    double duty;   /* a fan's drive %, NAN: not a fan */
    int64_t at;    /* ms, CLOCK_MONOTONIC, of the reading; 0: never */
};
#define MAX_READINGS 128

/* ---- chips this daemon reads itself (hwmon-chips.c): the PMBus devices
 * (READ_VIN, VOUT, IOUT, POUT, TEMPERATURE_1) but a VRM's temperature
 * (reimu-fand's), jc42 DIMM sensors. Reads only. REIMU_HWMON_FAKE=<dir>:
 * register files <dir>/i2c-<bus>/<addr, 2 hex digits> (256 bytes, a word is
 * reg, reg + 1 little-endian; with <addr>.words next to it, its words are
 * there, 2 * reg: PMBus commands), for the tests */
int chips_ours(const struct device *d);
/* appends the readings of d to r (n of max used), returns the new n */
int chips_read(struct device *d, struct reading *r, int n, int max, int64_t now);
unsigned long hw_transfers(void);
void hw_reset(void);

/* ---- the sensors (hwmon-model.c) */
struct entry
{
    /* the web UI's (reimu/sensors) */
    char kind[8]; /* temp fan in curr power */
    char group[128], name[96], src[16];
    double value, warn, crit, duty; /* NAN: none */
    /* D-Bus: /xyz/openbmc_project/sensors/<type>/<oname> */
    const char *type; /* temperature fan_tach voltage current power */
    const char *unit; /* xyz.openbmc_project.Sensor.Value.Unit.<unit> */
    char oname[96];   /* "" not published */
    double min, max;
    int64_t at;
};
#define MAX_ENTRIES 128
/* every sensor of the devices, with its reading (NAN where there is none) */
int model_build(const struct catalog *c, const struct reading *r, int nr, struct entry *e, int max);
/* "sensors":[...] of the web UI's document */
void model_json(const struct entry *e, int n, struct buf *b);
/* the Prometheus text format of the sensors with a value */
void model_metrics(const struct entry *e, int n, struct buf *b);

#endif
