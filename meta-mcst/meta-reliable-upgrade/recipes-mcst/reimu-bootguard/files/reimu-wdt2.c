/*
 * reimu-wdt2: access to the AST2400/AST2500 "2nd boot" watchdog (WDT2).
 *
 * With the dual boot hardware strap (SCU70[17]) WDT2 is armed at reset;
 * when it expires the SoC restarts from the second BMC flash chip and sets
 * bit 1 of the WDT2 timeout status register. U-Boot (REIMU) reloads it with
 * 300 seconds; reimu-bootguard disarms it once Linux is healthy.
 *
 *   reimu-wdt2 status    key=value lines: strap, secondary, enabled, reload,
 *                        counter (seconds = value / 1000000)
 *   reimu-wdt2 disarm    stop WDT2
 *   reimu-wdt2 ping      restart the countdown from the reload value
 *   reimu-wdt2 boot-primary
 *                        clear the timeout status and boot code selection
 *                        (the primary chip becomes CS0 again) and reboot at
 *                        once: after the switch the running system's flash is
 *                        gone, so nothing else may run in between
 *
 * REIMU_DEVMEM=<file> replaces /dev/mem for tests: the file holds the SCU
 * page at offset 0 and the WDT page at offset 0x1000.
 *
 * Copyright (c) 2026 MCST REIMU contributors. Licensed under GPLv2.
 */

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/reboot.h>
#include <unistd.h>

#define SCU_BASE      0x1e6e2000UL
#define WDT_BASE      0x1e785000UL
#define PAGE          0x1000UL

#define SCU_STRAP     0x70
#define STRAP_2ND_BOOT_WDT (1u << 17)

#define WDT2          0x20
#define WDT_COUNTER   0x00
#define WDT_RELOAD    0x04
#define WDT_RESTART   0x08
#define WDT_CTRL      0x0c
#define WDT_TIMEOUT   0x10
#define WDT_CLEAR     0x14
#define WDT_CLEAR_TIMEOUT_AND_BOOT_SELECTION (1u << 0)
#define WDT_CTRL_ENABLE (1u << 0)
#define WDT_TIMEOUT_SECONDARY (1u << 1)
#define WDT_RESTART_MAGIC 0x4755

static volatile uint32_t *map(int fd, off_t off)
{
    void *p = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, off);
    if (p == MAP_FAILED)
    {
        perror("reimu-wdt2: mmap");
        exit(2);
    }
    return p;
}

#define REG(base, off) ((base)[(off) / 4])

int main(int argc, char **argv)
{
    const char *test = getenv("REIMU_DEVMEM");
    int fd;
    volatile uint32_t *scu, *wdt;

    if (argc != 2)
    {
        fprintf(stderr, "Usage: %s status|disarm|ping|boot-primary\n", argv[0]);
        return 1;
    }
    fd = open(test ? test : "/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
    if (fd < 0)
    {
        perror("reimu-wdt2: open");
        return 2;
    }
    scu = map(fd, test ? 0 : (off_t)SCU_BASE);
    wdt = map(fd, test ? (off_t)PAGE : (off_t)WDT_BASE);

    if (!strcmp(argv[1], "status"))
    {
        uint32_t strap = REG(scu, SCU_STRAP);
        printf("strap=%d\n", (strap & STRAP_2ND_BOOT_WDT) ? 1 : 0);
        printf("secondary=%d\n", (REG(wdt, WDT2 + WDT_TIMEOUT) & WDT_TIMEOUT_SECONDARY) ? 1 : 0);
        printf("enabled=%d\n", (REG(wdt, WDT2 + WDT_CTRL) & WDT_CTRL_ENABLE) ? 1 : 0);
        printf("reload=%u\n", REG(wdt, WDT2 + WDT_RELOAD));
        printf("counter=%u\n", REG(wdt, WDT2 + WDT_COUNTER));
    }
    else if (!strcmp(argv[1], "disarm"))
    {
        REG(wdt, WDT2 + WDT_CTRL) &= ~WDT_CTRL_ENABLE;
        if (REG(wdt, WDT2 + WDT_CTRL) & WDT_CTRL_ENABLE)
        {
            fprintf(stderr, "reimu-wdt2: WDT2 is still enabled\n");
            return 3;
        }
    }
    else if (!strcmp(argv[1], "ping"))
    {
        REG(wdt, WDT2 + WDT_RESTART) = WDT_RESTART_MAGIC;
    }
    else if (!strcmp(argv[1], "boot-primary"))
    {
        sync();
        REG(wdt, WDT2 + WDT_CLEAR) = WDT_CLEAR_TIMEOUT_AND_BOOT_SELECTION;
        if (test)
        {
            printf("reboot\n");
            return 0;
        }
        reboot(RB_AUTOBOOT);
        perror("reimu-wdt2: reboot");
        return 4;
    }
    else
    {
        fprintf(stderr, "reimu-wdt2: unknown command %s\n", argv[1]);
        return 1;
    }
    return 0;
}
