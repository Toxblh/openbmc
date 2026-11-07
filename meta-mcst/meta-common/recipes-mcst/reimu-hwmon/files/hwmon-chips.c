/*
 * The chips reimu-hwmon reads itself: the ones no one else reads (reimu-fand
 * reads the fans' chips and the inputs of its control, and hands them over).
 *
 *   pmbus, ir3806x  READ_VIN (0x88), READ_VOUT (0x8b), READ_IOUT (0x8c),
 *                   READ_POUT (0x96) in LINEAR11 (VOUT in LINEAR16 by
 *                   VOUT_MODE 0x20), READ_TEMPERATURE_1 (0x8d) of a pmbus
 *                   device (a VRM's is reimu-fand's). IR38064 doesn't answer
 *                   VOUT_MODE and uses 2^-8 (its POUT, computed by the chip,
 *                   agrees). That is the voltage at its feedback pin: on
 *                   1Э8СВ the "+1V2" rails read ~0.52 V (VOUT_COMMAND 0x80 =
 *                   0.5 V, no VOUT_SCALE_LOOP), the rail itself is set by an
 *                   external divider. No PAGE or other write: the pmbus
 *                   driver's probe writes PAGE, these never do
 *   jc42            the temperature register (0x05, big-endian, 13 bits of
 *                   1/16 °C, two's complement; bits 15:13 are alarm flags)
 *
 * Over i2c-dev with I2C_SLAVE_FORCE (a driver may hold the address); single
 * SMBus transfers, each atomic on the adapter. A device that doesn't answer
 * at all is tried again only after RETRY_MS.
 *
 * Copyright (c) 2026 MCST REIMU contributors. Licensed under GPLv2.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "hwmon.h"

#define MAX_BUS  16
#define RETRY_MS 60000

static int s_init;
static const char *s_fake;
static int s_fd[MAX_BUS];
static unsigned long s_xfers;

static void hw_init(void)
{
    if (s_init) return;
    s_init = 1;
    s_fake = getenv("REIMU_HWMON_FAKE");
    if (s_fake && !*s_fake) s_fake = NULL;
    for (int i = 0; i < MAX_BUS; i++) s_fd[i] = -1;
}

void hw_reset(void)
{
    for (int i = 0; s_init && i < MAX_BUS; i++)
        if (s_fd[i] >= 0) close(s_fd[i]);
    s_init = 0;
    s_xfers = 0;
}

unsigned long hw_transfers(void)
{
    return s_xfers;
}

static int fake_rd(int bus, int addr, int reg, uint8_t *b, int n)
{
    char p[512];
    snprintf(p, sizeof(p), "%s/i2c-%d/%02x", s_fake, bus, addr);
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    int r = pread(fd, b, n, reg) == n ? 0 : -1;
    close(fd);
    return r;
}

/* the bus's i2c-dev, set to the address */
static int bus_fd(int bus, int addr)
{
    if (bus < 0 || bus >= MAX_BUS) return -1;
    if (s_fd[bus] < 0)
    {
        char p[32];
        snprintf(p, sizeof(p), "/dev/i2c-%d", bus);
        s_fd[bus] = open(p, O_RDWR | O_CLOEXEC);
        if (s_fd[bus] < 0) return -1;
    }
    return ioctl(s_fd[bus], I2C_SLAVE_FORCE, addr) < 0 ? -1 : s_fd[bus];
}

static int smbus_read(int bus, int addr, int reg, int size, union i2c_smbus_data *d)
{
    int fd = bus_fd(bus, addr);
    if (fd < 0) return -1;
    struct i2c_smbus_ioctl_data a = { I2C_SMBUS_READ, (uint8_t)reg, size, d };
    return ioctl(fd, I2C_SMBUS, &a);
}

static int rd_byte(int bus, int addr, int reg)
{
    hw_init();
    s_xfers++;
    if (s_fake)
    {
        uint8_t b;
        return fake_rd(bus, addr, reg, &b, 1) ? -1 : b;
    }
    union i2c_smbus_data d;
    return smbus_read(bus, addr, reg, I2C_SMBUS_BYTE_DATA, &d) < 0 ? -1 : d.byte;
}

/* SMBus word: low byte first. The fake: <addr>.words holds a word per
 * command (at 2 * reg), as PMBus commands are; else reg, reg + 1 of the
 * register file (an EEPROM in QEMU) */
static int rd_word(int bus, int addr, int reg)
{
    hw_init();
    s_xfers++;
    if (s_fake)
    {
        uint8_t b[2];
        char p[512];
        snprintf(p, sizeof(p), "%s/i2c-%d/%02x.words", s_fake, bus, addr);
        int fd = open(p, O_RDONLY | O_CLOEXEC);
        if (fd >= 0)
        {
            int ok = pread(fd, b, 2, 2 * reg) == 2;
            close(fd);
            return ok ? b[0] | b[1] << 8 : -1;
        }
        return fake_rd(bus, addr, reg, b, 2) ? -1 : b[0] | b[1] << 8;
    }
    union i2c_smbus_data d;
    return smbus_read(bus, addr, reg, I2C_SMBUS_WORD_DATA, &d) < 0 ? -1 : d.word;
}

/* PMBus LINEAR11: 5-bit exponent, 11-bit mantissa, both signed */
static double linear11(int w)
{
    int e = (w >> 11) & 0x1f, m = w & 0x7ff;
    if (e & 0x10) e -= 32;
    if (m & 0x400) m -= 2048;
    return ldexp(m, e);
}

static int add(struct reading *r, int n, int max, const struct device *d, const char *input, double v, int64_t now)
{
    if (n >= max) return n;
    struct reading *x = &r[n];
    snprintf(x->dev, sizeof(x->dev), "%d-%04x", d->bus, d->addr);
    snprintf(x->input, sizeof(x->input), "%s", input);
    x->value = v;
    x->duty = NAN;
    x->at = now;
    return n + 1;
}

int chips_ours(const struct device *d)
{
    return d->kind == DK_PMBUS || d->kind == DK_IR3806X || d->kind == DK_JC42;
}

int chips_read(struct device *d, struct reading *r, int n, int max, int64_t now)
{
    if (!chips_ours(d) || (d->retry_at && now < d->retry_at)) return n;
    int b = d->bus, a = d->addr, n0 = n, w;
    if (d->kind == DK_JC42)
    {
        if ((w = rd_word(b, a, 0x05)) >= 0)
        {
            int raw = ((w & 0xff) << 8 | w >> 8) & 0x1fff; /* big-endian on the wire */
            n = add(r, n, max, d, "temp1", ((raw ^ 0x1000) - 0x1000) / 16.0, now);
        }
    }
    else
    {
        int mode = rd_byte(b, a, 0x20), exp = -8, vout = 1;
        if (mode >= 0)
        {
            if ((mode >> 5) == 0) exp = (mode & 0x10) ? (mode & 0x1f) - 32 : (mode & 0x1f);
            else vout = 0; /* VID or direct: needs the device's tables */
        }
        else if (d->kind != DK_IR3806X) vout = 0;
        w = rd_word(b, a, 0x88);
        if (w < 0 && mode < 0)
        {
            d->retry_at = now + RETRY_MS; /* not there (2 transfers, not 6) */
            return n;
        }
        if (w >= 0) n = add(r, n, max, d, "vin", linear11(w), now);
        if (vout && (w = rd_word(b, a, 0x8b)) >= 0) n = add(r, n, max, d, "vout1", ldexp(w, exp), now);
        if ((w = rd_word(b, a, 0x8c)) >= 0) n = add(r, n, max, d, "iout1", linear11(w), now);
        if ((w = rd_word(b, a, 0x96)) >= 0) n = add(r, n, max, d, "pout1", linear11(w), now);
        if (d->kind == DK_PMBUS && (w = rd_word(b, a, 0x8d)) >= 0) n = add(r, n, max, d, "temp1", linear11(w), now);
    }
    d->retry_at = n == n0 ? now + RETRY_MS : 0;
    return n;
}
