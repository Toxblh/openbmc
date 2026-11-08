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
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

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

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "p2a diag | rd ADDR [N] | find ADDR LEN TEXT | wdtreset SECONDS\n"); return 2; }
    int fd = open(RES, O_RDWR | O_SYNC);
    if (fd < 0) { perror(RES); return 1; }
    bar = mmap(0, 0x20000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (bar == MAP_FAILED) { perror("mmap"); return 1; }
    printf("P2A: enable=%08x base=%08x\n", *(volatile uint32_t *)(bar + 0xf000), *(volatile uint32_t *)(bar + 0xf004));
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
