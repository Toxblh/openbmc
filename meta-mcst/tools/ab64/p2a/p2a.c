/* p2a: the AST2500 BMC's AHB from the host, through the VGA device's BAR1
 * (P2A bridge: 0xF000 enable, 0xF004 the 64 KiB window's base, the window
 * at BAR1 + 0x10000). Root. The host's ast driver uses the same window: the
 * base is set before every access and checked after.
 *
 *   p2a diag                 SoC id, WDT1-3 (counter twice, 2 s apart), SCU reset/strap
 *   p2a rd ADDR [N]          N words
 *   p2a find ADDR LEN TEXT   search BMC memory for TEXT, print 400 bytes around each hit
 *   p2a dump ADDR LEN FILE   raw bytes (readable by everyone)
 *   p2a wdtreset SECONDS [MASK]  WDT2: reset the BMC SoC in SECONDS (asks for "yes" on stdin);
 *                            MASK: WDT2 reset mask (0x1e78503c), the blocks it resets (default 0x023ffff3)
 *
 * The BMC's own SPI flash (FMC, CE0: its U-Boot, both slots, rwfs), a way back
 * without a programmer when the BMC doesn't boot (its U-Boot broken):
 *
 *   p2a flash-id               the chip's JEDEC ID (user mode, 9Fh)
 *   p2a flash-read OFF LEN FILE   LEN bytes at OFF (the memory window, no mode change)
 *   p2a flash-write OFF FILE [--yes] [--force]
 *                            FILE at OFF (4 KiB aligned): only the 4 KiB blocks
 *                            that differ, each erased (21h) and programmed (12h,
 *                            256-byte pages, 4-byte addresses), all read back
 *                            and compared; asks for "yes" unless --yes. Refused
 *                            while the BMC runs (WDT1 counting: its Linux uses
 *                            the flash too) or the host's ast driver is loaded
 *                            (it moves the bridge's window) unless --force.
 *
 * FMC user mode as Linux's spi-aspeed-smc does it: CE0 control (0x1e620010)
 * 0x7 (user mode, CE# high), 0x3 (CE# low), bytes written to/read from the CE0
 * window (0x20000000); the write enable of CE0 (FMC00 bit 16); the controller
 * as it was after. Micron N25Q512 (20BA20, two dies): ready = status WIP 0
 * and flag status (70h) bit 7.
 *
 * Built with -DDEVMEM it runs on the BMC itself over /dev/mem (the same code
 * tested in QEMU, test_regressions_qemu p2aflash).
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef DEVMEM
static int memfd;
static volatile uint8_t *mwin;
static uint32_t mcur = 0xffffffff;
static volatile uint8_t *at(uint32_t a)
{
    uint32_t b = a & 0xffff0000u;
    if (b != mcur) {
        if (mwin) munmap((void *)mwin, 0x10000);
        mwin = mmap(0, 0x10000, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, b);
        if (mwin == MAP_FAILED) { perror("mmap /dev/mem"); exit(1); }
        mcur = b;
    }
    return mwin + (a & 0xffff);
}
static uint32_t rd(uint32_t a) { return *(volatile uint32_t *)at(a & ~3u); }
static int wr(uint32_t a, uint32_t v) { *(volatile uint32_t *)at(a & ~3u) = v; return 0; }
static uint8_t rd8(uint32_t a) { return *at(a); }
static int wr8(uint32_t a, uint8_t v) { *at(a) = v; return 0; }
#else
#define RES "/sys/bus/pci/devices/0000:05:00.0/resource1"
static volatile uint8_t *bar;
static uint32_t cur = 0xffffffff;

static void win(uint32_t a)
{
    uint32_t b = a & 0xffff0000u;
    if (b != cur || *(volatile uint32_t *)(bar + 0xf004) != b) {
        *(volatile uint32_t *)(bar + 0xf004) = b;
        *(volatile uint32_t *)(bar + 0xf000) = 1;
        cur = b;
    }
}
static uint32_t rd(uint32_t a)
{
    for (;;) {
        win(a);
        uint32_t v = *(volatile uint32_t *)(bar + 0x10000 + (a & 0xfffc));
        if (*(volatile uint32_t *)(bar + 0xf004) == (a & 0xffff0000u)) return v;
        cur = 0xffffffff; /* the ast driver moved it: again */
    }
}
static int wr(uint32_t a, uint32_t v)
{
    win(a);
    if (*(volatile uint32_t *)(bar + 0xf004) != (a & 0xffff0000u)) return -1;
    *(volatile uint32_t *)(bar + 0x10000 + (a & 0xfffc)) = v;
    return 0;
}
static uint8_t rd8(uint32_t a)
{
    for (;;) {
        win(a);
        uint8_t v = *(bar + 0x10000 + (a & 0xffff));
        if (*(volatile uint32_t *)(bar + 0xf004) == (a & 0xffff0000u)) return v;
        cur = 0xffffffff;
    }
}
static int wr8(uint32_t a, uint8_t v)
{
    win(a);
    if (*(volatile uint32_t *)(bar + 0xf004) != (a & 0xffff0000u)) return -1;
    *(bar + 0x10000 + (a & 0xffff)) = v;
    return 0;
}
#endif

/* ---------------------------------------------------------------- BMC flash */
#define FMC 0x1e620000u
#define CE0 0x20000000u
#define FLASH_SIZE 0x4000000u
static uint32_t save00, save10;
static int micron, werr;
static void W(uint32_t a, uint32_t v) { if (wr(a, v)) werr = 1; }
static void W8(uint32_t a, uint8_t v) { if (wr8(a, v)) werr = 1; }
static void user_begin(void)
{
    save00 = rd(FMC);
    save10 = rd(FMC + 0x10);
    W(FMC, save00 | 1u << 16);
    W(FMC + 0x10, 0x7);
}
static void user_end(void)
{
    W(FMC + 0x10, save10 | 0x4);
    W(FMC + 0x10, save10);
    W(FMC, save00);
}
static void xfer(const uint8_t *out, int n, uint8_t *in, int m)
{
    W(FMC + 0x10, 0x3);
    for (int i = 0; i < n; i++) W8(CE0, out[i]);
    for (int i = 0; i < m; i++) in[i] = rd8(CE0);
    W(FMC + 0x10, 0x7);
}
static uint8_t status(uint8_t c) { uint8_t v; xfer(&c, 1, &v, 1); return v; }
static int ready(int ms)
{
    for (int t = 0; t < ms; t++) {
        if (!(status(0x05) & 1) && (!micron || status(0x70) & 0x80)) return 0;
        usleep(1000);
    }
    return -1;
}
static int wren(void)
{
    uint8_t c = 0x06;
    xfer(&c, 1, 0, 0);
    return status(0x05) & 2 ? 0 : -1;
}
static uint32_t jedec(void)
{
    uint8_t c = 0x9f, id[3];
    user_begin();
    xfer(&c, 1, id, 3);
    user_end();
    return id[0] << 16 | id[1] << 8 | id[2];
}
static void flash_rd(uint32_t off, uint8_t *buf, uint32_t len)
{
    for (uint32_t i = 0; i < len; i += 4) { uint32_t v = rd(CE0 + off + i); memcpy(buf + i, &v, len - i < 4 ? len - i : 4); }
}
/* one 4 KiB block: erase, program; -1 on a timeout or no write enable */
static int flash_block(uint32_t off, const uint8_t *d)
{
    uint8_t c[5] = { 0x21, off >> 24, off >> 16, off >> 8, off };
    if (wren()) { fprintf(stderr, "%08x: write enable not set (write protected?)\n", off); return -1; }
    xfer(c, 5, 0, 0);
    if (ready(3000)) { fprintf(stderr, "%08x: erase timed out\n", off); return -1; }
    for (uint32_t p = 0; p < 4096; p += 256) {
        int blank = 1;
        for (int i = 0; i < 256; i++) if (d[p + i] != 0xff) { blank = 0; break; }
        if (blank) continue;
        uint32_t a = off + p;
        uint8_t h[5] = { 0x12, a >> 24, a >> 16, a >> 8, a };
        if (wren()) { fprintf(stderr, "%08x: write enable not set\n", a); return -1; }
        W(FMC + 0x10, 0x3);
        for (int i = 0; i < 5; i++) W8(CE0, h[i]);
        for (int i = 0; i < 256; i++) W8(CE0, d[p + i]);
        W(FMC + 0x10, 0x7);
        if (ready(100)) { fprintf(stderr, "%08x: program timed out\n", a); return -1; }
    }
    return 0;
}
static int ask(const char *what)
{
    char y[8] = "";
    printf("%s? type yes: ", what); fflush(stdout);
    return fgets(y, sizeof(y), stdin) && !strncmp(y, "yes", 3);
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "p2a diag | rd ADDR [N] | find ADDR LEN TEXT | wdtreset SECONDS | flash-id | flash-read OFF LEN FILE | flash-write OFF FILE [--yes] [--force]\n"); return 2; }
#ifdef DEVMEM
    memfd = open("/dev/mem", O_RDWR | O_SYNC);
    if (memfd < 0) { perror("/dev/mem"); return 1; }
#else
    int fd = open(RES, O_RDWR | O_SYNC);
    if (fd < 0) { perror(RES); return 1; }
    bar = mmap(0, 0x20000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (bar == MAP_FAILED) { perror("mmap"); return 1; }
    printf("P2A: enable=%08x base=%08x\n", *(volatile uint32_t *)(bar + 0xf000), *(volatile uint32_t *)(bar + 0xf004));
#endif
    if (!strcmp(argv[1], "flash-id")) {
        uint32_t id = jedec();
        printf("BMC flash JEDEC ID %06x%s\n", id, werr ? " (the bridge's window moved: retry)" : "");
        return id == 0 || id == 0xffffff || werr;
    }
    if (!strcmp(argv[1], "flash-read") && argc >= 5) {
        uint32_t off = strtoul(argv[2], 0, 0), len = strtoul(argv[3], 0, 0);
        if (off >= FLASH_SIZE || len > FLASH_SIZE - off) { fprintf(stderr, "beyond the flash\n"); return 2; }
        uint8_t *b = malloc(len + 4);
        flash_rd(off, b, len);
        FILE *f = fopen(argv[4], "wb");
        if (!f || fwrite(b, 1, len, f) != len) { perror(argv[4]); return 1; }
        fclose(f);
        printf("%u bytes of the BMC flash at %08x to %s\n", len, off, argv[4]);
        return 0;
    }
    if (!strcmp(argv[1], "flash-write") && argc >= 4) {
        int yes = 0, force = 0;
        for (int i = 4; i < argc; i++) { if (!strcmp(argv[i], "--yes")) yes = 1; if (!strcmp(argv[i], "--force")) force = 1; }
        uint32_t off = strtoul(argv[2], 0, 0);
        FILE *f = fopen(argv[3], "rb");
        if (!f) { perror(argv[3]); return 1; }
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (off % 4096 || n <= 0 || off >= FLASH_SIZE || (uint32_t)n > FLASH_SIZE - off) { fprintf(stderr, "OFF must be 4 KiB aligned, FILE within the 64 MiB flash\n"); return 2; }
        uint32_t len = (n + 4095) / 4096 * 4096;
        uint8_t *want = malloc(len), *have = malloc(len);
        flash_rd(off, want, len);              /* the tail of the last block stays as it is */
        if (fread(want, 1, n, f) != (size_t)n) { perror(argv[3]); return 1; }
        fclose(f);
        if (!force) {
            uint32_t c0 = rd(0x1e785000); sleep(1);
            if ((rd(0x1e78500c) & 1) && rd(0x1e785000) != c0) { fprintf(stderr, "the BMC runs (WDT1 counting): its Linux uses this flash too. --force to write anyway\n"); return 1; }
#ifndef DEVMEM
            if (!access("/sys/module/ast", F_OK)) { fprintf(stderr, "the host's ast driver is loaded (it moves the bridge's window): rmmod ast, or --force\n"); return 1; }
#endif
        }
        uint32_t id = jedec();
        micron = id >> 16 == 0x20;
        flash_rd(off, have, len);
        int todo = 0;
        for (uint32_t b = 0; b < len; b += 4096) todo += !!memcmp(have + b, want + b, 4096);
        printf("BMC flash %06x: %u bytes at %08x, %d of %u blocks of 4 KiB differ\n", id, len, off, todo, len / 4096);
        if (!todo) { printf("nothing to write\n"); return 0; }
        if (!id || id == 0xffffff) { fprintf(stderr, "no flash answers (JEDEC %06x)\n", id); return 1; }
        if (!yes && !ask("write them")) { printf("not done\n"); return 1; }
        for (int t = 1; t <= 3 && todo; t++) {
            user_begin();
            int bad = 0;
            for (uint32_t b = 0; b < len; b += 4096)
                if (memcmp(have + b, want + b, 4096) && flash_block(off + b, want + b)) bad = 1;
            user_end();
            flash_rd(off, have, len);
            todo = 0;
            for (uint32_t b = 0; b < len; b += 4096) todo += !!memcmp(have + b, want + b, 4096);
            printf("try %d: %s, %d block(s) still differ%s\n", t, bad ? "errors" : "written", todo, werr ? " (the bridge's window moved)" : "");
            werr = 0;
        }
        printf(todo ? "NOT written: read it back and try again\n" : "written and read back: the same\n");
        return !!todo;
    }
    if (!strcmp(argv[1], "diag")) {
        printf("SCU7C silicon id %08x (AST2500 A1/A2: 0x04030303)\n", rd(0x1e6e207c));
        printf("SCU3C sys reset ctrl/status %08x  SCU70 strap %08x\n", rd(0x1e6e203c), rd(0x1e6e2070));
        for (int w = 0; w < 3; w++) {
            uint32_t b = 0x1e785000 + w * 0x20;
            printf("WDT%d counter %08x reload %08x ctrl %08x timeout-status %08x\n", w + 1, rd(b), rd(b + 4), rd(b + 0xc), rd(b + 0x10));
        }
        sleep(2);
        for (int w = 0; w < 3; w++) printf("WDT%d counter after 2 s %08x\n", w + 1, rd(0x1e785000 + w * 0x20));
        printf("UART5 (ttyS4) LSR %08x\n", rd(0x1e784014));
        return 0;
    }
    if (!strcmp(argv[1], "rd") && argc >= 3) {
        uint32_t a = strtoul(argv[2], 0, 0); int n = argc > 3 ? atoi(argv[3]) : 1;
        for (int i = 0; i < n; i++) printf("%08x: %08x\n", a + 4 * i, rd(a + 4 * i));
        return 0;
    }
    if (!strcmp(argv[1], "find") && argc >= 5) {
        uint32_t a = strtoul(argv[2], 0, 0), len = strtoul(argv[3], 0, 0);
        const char *t = argv[4]; size_t tl = strlen(t);
        static uint8_t buf[0x10000 + 1024];
        int hits = 0;
        for (uint32_t off = 0; off < len && hits < 20; off += 0x10000) {
            for (uint32_t i = 0; i < 0x10000 + 1024; i += 4) { uint32_t v = rd(a + off + i); memcpy(buf + i, &v, 4); }
            for (uint32_t i = 0; i < 0x10000 && hits < 20; i++)
                if (!memcmp(buf + i, t, tl)) {
                    hits++;
                    printf("--- %08x\n", a + off + i);
                    uint32_t s = i > 200 ? i - 200 : 0;
                    for (uint32_t k = s; k < s + 600 && k < sizeof(buf); k++) putchar(buf[k] >= 32 && buf[k] < 127 ? buf[k] : (buf[k] == '\n' ? '\n' : '.'));
                    putchar('\n');
                }
        }
        printf("%d hits\n", hits);
        return 0;
    }
    if (!strcmp(argv[1], "dump") && argc >= 5) {
        uint32_t a = strtoul(argv[2], 0, 0), len = strtoul(argv[3], 0, 0);
        FILE *f = fopen(argv[4], "wb");
        if (!f) { perror(argv[4]); return 1; }
        for (uint32_t i = 0; i < len; i += 4) { uint32_t v = rd(a + i); fwrite(&v, 4, 1, f); }
        fclose(f);
        chmod(argv[4], 0644);
        printf("%u bytes from %08x to %s\n", len, a, argv[4]);
        return 0;
    }
    if (!strcmp(argv[1], "wdtreset") && argc >= 3) {
        int s = atoi(argv[2]);
        if (s < 1 || s > 60) { fprintf(stderr, "1..60 s\n"); return 2; }
        char y[8] = "";
        printf("reset the BMC SoC by WDT2 in %d s%s (the host may be reset too)? type yes: ", s, argc >= 4 ? " with that mask" : ""); fflush(stdout);
        if (!fgets(y, sizeof(y), stdin) || strncmp(y, "yes", 3)) { printf("not done\n"); return 1; }
        uint32_t b = 0x1e785020;
        if (argc >= 4) {
            uint32_t m = strtoul(argv[3], 0, 0);
            if (!(m & 1)) { fprintf(stderr, "mask without bit 0 (the ARM CPU): the BMC would not restart\n"); return 2; }
            if (wr(b + 0x1c, m)) { printf("window moved: not done\n"); return 1; }
        }
        printf("WDT2 reset mask %08x\n", rd(b + 0x1c));
        if (wr(b + 0xc, 0) || wr(b + 4, (uint32_t)s * 1000000u) || wr(b + 8, 0x4755) || wr(b + 0xc, 0x13)) { printf("window moved: not done\n"); return 1; }
        printf("WDT2 reload %08x ctrl %08x counter %08x: armed\n", rd(b + 4), rd(b + 0xc), rd(b));
        return 0;
    }
    fprintf(stderr, "?\n");
    return 2;
}
