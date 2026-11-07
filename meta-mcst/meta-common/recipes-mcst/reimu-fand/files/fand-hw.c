/* reimu-fand hardware access: SMBus transfers over i2c-dev, the fault LED.
 *
 * Every device is reached by its address with I2C_SLAVE_FORCE (an image
 * with hwmon-instantiator binds the lm96163s to the lm63 driver; the A/B
 * image binds no driver to them: this daemon is their only reader, and hands
 * what it read to reimu-hwmon); single SMBus transfers are atomic on the
 * adapter. Writes go to the chips directly, never through the lm63 sysfs
 * files: that driver ignores I2C errors on writes and caches the fan config
 * register, so only a direct write and its read back say whether the fan got
 * its duty.
 *
 * REIMU_FAND_FAKE=<dir> replaces the hardware by files, for the tests: see
 * fand.h.
 *
 * Copyright (c) 2026 MCST REIMU contributors. Licensed under GPLv2.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "fand.h"

#define MAX_BUS 16

static int s_init;
static const char *s_fake; /* NULL: real hardware */
static int s_fd[MAX_BUS];
static unsigned long s_xfers;

static void hw_init(void)
{
    if (s_init) return;
    s_init = 1;
    s_fake = getenv("REIMU_FAND_FAKE");
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

/* ------------------------------------------------------------------ fake */
static int fake_open(int bus, int addr, int wr)
{
    char p[512];
    snprintf(p, sizeof(p), "%s/i2c-%d/%02x", s_fake, bus, addr);
    int fd = open(p, (wr ? O_RDWR : O_RDONLY) | O_CLOEXEC);
    if (fd < 0) errno = ENXIO;
    return fd;
}

/* <dev>.lm96163: behaves as that chip where it matters
 *   0x4C (PWM value) and the LUT PWM entries keep 6 bits (8 in the
 *   high-resolution mode, see fand.c)
 *   tachometer: the count is the "live" bytes 0x46/0x47; reading 0x46
 *   latches the MSB, which 0x47 returns (0xff until latched)
 *   0x03, 0x04, 0x05, 0x07, 0x08 are written at 0x09, 0x0a, 0x0b, 0x0d, 0x0e
 *   (both then read the value), a write to the read address is ignored;
 *   <dev>.noalias: plain registers (an EEPROM in QEMU)
 *   0x4A keeps 6 bits, 0x4D 5 bits, 0x4A bit 4 inverts the PWM (.fan)
 *   the remote offset 0x11 is in the reading 0x01
 *   <dev>.no-polarity: 0x4A bit 4 can't be changed */
static int fake_is_lm96163(int bus, int addr)
{
    char p[512];
    snprintf(p, sizeof(p), "%s/i2c-%d/%02x.lm96163", s_fake, bus, addr);
    return !access(p, F_OK);
}

static int fake_latch(int bus, int addr, int set)
{
    char p[512];
    snprintf(p, sizeof(p), "%s/i2c-%d/%02x.latch", s_fake, bus, addr);
    uint8_t v = 0xff;
    if (set >= 0)
    {
        int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        v = set;
        if (fd >= 0 && write(fd, &v, 1) != 1) v = 0xff;
        if (fd >= 0) close(fd);
        return v;
    }
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd >= 0)
    {
        if (read(fd, &v, 1) != 1) v = 0xff;
        close(fd);
    }
    return v;
}

static int fake_pwm_mask(int fd);

/* <dev>.fan next to a <dev>.lm96163: the X37 CPU fan as measured on the
 * board (sysfs fan1_input, 2 s apart): its tachometer count follows the duty
 * the chip has, with that noise and those dropouts to 0. "stall" in the file:
 * a stopped fan (count 0xfffc) */
static double fake_fan_rpm(int duty, unsigned k, int stalled, int mode)
{
    /* 0x4A bits 1:0 = 11, least effect (8 samples, 1.5 s apart) */
    static const double l30[] = { 677, 1021, 602, 1018, 814, 816, 0, 1028 };
    static const double l40[] = { 659, 1293, 1380, 1176, 591, 721, 1219, 677 };
    static const double l50[] = { 1654, 1473, 1630, 1646, 1480, 1480, 1646, 1505 };
    /* 10, most accurate */
    static const double a30[] = { 884, 933, 901, 945, 901, 884, 915, 874 };
    static const double a40[] = { 1034, 1009, 1047, 1002, 997, 1015, 1039, 1043 };
    static const double a50[] = { 1557, 1558, 1564, 1558, 1557, 1560, 1564, 1564 };
    /* 00, traditional: the earlier measurement, with its dropouts */
    static const double t30[] = { 2402, 1014, 1274, 749, 958, 1054, 1014, 749 };
    static const double t40[] = { 696, 0, 1191, 595, 0, 1102, 0, 595 };
    const double *p30 = mode == 2 ? a30 : mode == 3 ? l30 : t30, *p40 = mode == 2 ? a40 : mode == 3 ? l40 : t40;
    const double *p50 = mode == 2 ? a50 : l50;
    if (stalled) return 0;
    if (duty < 35) return p30[k % 8];
    if (duty < 45) return p40[k % 8];
    if (duty < 58) return p50[k % 8];
    return 2700 - (k % 3) * 10;
}

static void fake_fan(int fd, int bus, int addr)
{
    char p[512], s[16] = "";
    snprintf(p, sizeof(p), "%s/i2c-%d/%02x.fan", s_fake, bus, addr);
    int ff = open(p, O_RDONLY | O_CLOEXEC);
    if (ff < 0) return;
    ssize_t n = read(ff, s, sizeof(s) - 1);
    close(ff);
    s[n > 0 ? n : 0] = 0;
    static unsigned k;
    uint8_t pwm, cfg, freq;
    if (pread(fd, &pwm, 1, 0x4c) != 1 || pread(fd, &cfg, 1, 0x4a) != 1 || pread(fd, &freq, 1, 0x4d) != 1) return;
    int max = fake_pwm_mask(fd) == 0xff ? 255 : 2 * (freq & 0x1f ? freq & 0x1f : 1);
    int duty = pwm >= max ? 100 : pwm * 100 / max;
    if (cfg & 0x10) duty = 100 - duty; /* inverted PWM: full scale stops it */
    double rpm = duty < 10 ? 0 : fake_fan_rpm(duty, k++, !strncmp(s, "stall", 5), cfg & 3);
    int count = rpm <= 0 ? 0xfffc : ((int)(5400000 / rpm) & 0xfffc);
    uint8_t c2[2] = { count & 0xff, count >> 8 };
    if (pwrite(fd, c2, 2, 0x46) != 2) return;
}

static int fake_rd(int bus, int addr, int reg, uint8_t *b, int n)
{
    int fd = fake_open(bus, addr, reg == 0x46 && n == 1 && fake_is_lm96163(bus, addr));
    if (fd < 0) return -1;
    if (reg == 0x46 && n == 1 && fake_is_lm96163(bus, addr)) fake_fan(fd, bus, addr);
    int r = pread(fd, b, n, reg) == n ? 0 : -1;
    if (!r && n == 1 && fake_is_lm96163(bus, addr))
    {
        uint8_t msb;
        if (reg == 0x46 && pread(fd, &msb, 1, 0x47) == 1) fake_latch(bus, addr, msb);
        if (reg == 0x47) b[0] = fake_latch(bus, addr, -1);
    }
    close(fd);
    if (r) errno = EIO;
    return r;
}

static int fake_pwm_mask(int fd)
{
    uint8_t cfg, freq, enh;
    if (pread(fd, &cfg, 1, 0x4a) != 1 || pread(fd, &freq, 1, 0x4d) != 1 || pread(fd, &enh, 1, 0x45) != 1) return 0x3f;
    return (enh & 0x10) && !(cfg & 0x08) && freq == 8 ? 0xff : 0x3f;
}

static int fake_wr(int bus, int addr, int reg, int val)
{
    int fd = fake_open(bus, addr, 1);
    if (fd < 0) return -1;
    /* <dev>.ro: the chip acknowledges writes but doesn't take them */
    char p[512];
    snprintf(p, sizeof(p), "%s/i2c-%d/%02x.ro", s_fake, bus, addr);
    if (!access(p, F_OK))
    {
        close(fd);
        return 0;
    }
    uint8_t b = val;
    if (fake_is_lm96163(bus, addr))
    {
        static const uint8_t rd[] = { 0x03, 0x04, 0x05, 0x07, 0x08 }, wr[] = { 0x09, 0x0a, 0x0b, 0x0d, 0x0e };
        snprintf(p, sizeof(p), "%s/i2c-%d/%02x.noalias", s_fake, bus, addr);
        int alias = access(p, F_OK) != 0;
        for (int i = 0; alias && i < 5; i++)
        {
            if (reg == rd[i])
            {
                close(fd);
                return 0;
            }
            if (reg == wr[i] && pwrite(fd, &b, 1, rd[i]) != 1) break;
        }
        if (reg == 0x4a) b &= 0x3f;
        if (reg == 0x4d) b &= 0x1f;
        uint8_t was;
        snprintf(p, sizeof(p), "%s/i2c-%d/%02x.no-polarity", s_fake, bus, addr);
        if (reg == 0x4a && !access(p, F_OK) && pread(fd, &was, 1, 0x4a) == 1) b = (b & ~0x10) | (was & 0x10);
        uint8_t off, temp;
        if (reg == 0x11 && pread(fd, &off, 1, 0x11) == 1 && pread(fd, &temp, 1, 0x01) == 1)
        {
            temp = (uint8_t)((int8_t)temp + (int8_t)b - (int8_t)off);
            if (pwrite(fd, &temp, 1, 0x01) != 1) errno = EIO;
        }
    }
    if (fake_is_lm96163(bus, addr) && (reg == 0x4c || (reg >= 0x51 && reg <= 0x67 && (reg & 1)))) b &= fake_pwm_mask(fd);
    /* <dev>.no-tachmode: a chip that doesn't take 0x4A bits 1:0 */
    snprintf(p, sizeof(p), "%s/i2c-%d/%02x.no-tachmode", s_fake, bus, addr);
    uint8_t old;
    if (reg == 0x4a && !access(p, F_OK) && pread(fd, &old, 1, 0x4a) == 1) b = (b & ~0x03) | (old & 0x03);
    int r = pwrite(fd, &b, 1, reg) == 1 ? 0 : -1;
    close(fd);
    if (r) errno = EIO;
    return r;
}

/* ------------------------------------------------------------------ i2c-dev */
static int bus_fd(int bus, int addr)
{
    if (bus < 0 || bus >= MAX_BUS || addr < 0x03 || addr > 0x77)
    {
        errno = EINVAL;
        return -1;
    }
    if (s_fd[bus] < 0)
    {
        char p[32];
        snprintf(p, sizeof(p), "/dev/i2c-%d", bus);
        s_fd[bus] = open(p, O_RDWR | O_CLOEXEC);
        if (s_fd[bus] < 0) return -1;
    }
    if (ioctl(s_fd[bus], I2C_SLAVE_FORCE, addr) < 0)
    {
        int e = errno;
        close(s_fd[bus]);
        s_fd[bus] = -1;
        errno = e;
        return -1;
    }
    return s_fd[bus];
}

static int smbus(int fd, char rw, uint8_t cmd, int size, union i2c_smbus_data *d)
{
    struct i2c_smbus_ioctl_data a = { rw, cmd, size, d };
    return ioctl(fd, I2C_SMBUS, &a);
}

int64_t hw_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

int hw_rd_byte(int bus, int addr, int reg)
{
    hw_init();
    s_xfers++;
    if (s_fake)
    {
        uint8_t b;
        return fake_rd(bus, addr, reg, &b, 1) ? -1 : b;
    }
    int fd = bus_fd(bus, addr);
    union i2c_smbus_data d;
    if (fd < 0 || smbus(fd, I2C_SMBUS_READ, reg, I2C_SMBUS_BYTE_DATA, &d) < 0) return -1;
    return d.byte;
}

int hw_wr_byte(int bus, int addr, int reg, int val)
{
    hw_init();
    s_xfers++;
    if (s_fake) return fake_wr(bus, addr, reg, val);
    int fd = bus_fd(bus, addr);
    union i2c_smbus_data d;
    d.byte = val;
    if (fd < 0 || smbus(fd, I2C_SMBUS_WRITE, reg, I2C_SMBUS_BYTE_DATA, &d) < 0) return -1;
    return 0;
}

int hw_rd_word(int bus, int addr, int reg)
{
    hw_init();
    s_xfers++;
    if (s_fake)
    {
        uint8_t b[2];
        return fake_rd(bus, addr, reg, b, 2) ? -1 : b[0] | b[1] << 8;
    }
    int fd = bus_fd(bus, addr);
    union i2c_smbus_data d;
    if (fd < 0 || smbus(fd, I2C_SMBUS_READ, reg, I2C_SMBUS_WORD_DATA, &d) < 0) return -1;
    return d.word;
}

/* one read message of 2 bytes, as overheatd and reimu-pnsd read the PCS */
int hw_rd_plain2(int bus, int addr, uint8_t b[2])
{
    hw_init();
    s_xfers++;
    if (s_fake) return fake_rd(bus, addr, 0, b, 2);
    int fd = bus_fd(bus, addr);
    if (fd < 0) return -1;
    return read(fd, b, 2) == 2 ? 0 : -1;
}

/* ------------------------------------------------------------------ LED */
static int write_file(const char *path, const char *s)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC | O_TRUNC);
    if (fd < 0) return -1;
    size_t n = strlen(s);
    int r = write(fd, s, n) == (ssize_t)n ? 0 : -1;
    if (close(fd)) r = -1;
    return r;
}

int hw_led(const char *name, int on)
{
    hw_init();
    char p[512];
    const char *root = s_fake ? s_fake : "/sys/class";
    snprintf(p, sizeof(p), "%s/leds/%s/trigger", root, name);
    /* a trigger of its own (default-on) rather than brightness: as overheatd
     * and setled do with theirs */
    return write_file(p, on ? "default-on\n" : "none\n");
}
