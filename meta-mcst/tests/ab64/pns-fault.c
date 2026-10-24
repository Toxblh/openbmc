/* LD_PRELOAD for test_pns_qemu.py: writes to the PNS flash that go wrong.
 * PNS_FAULT is a comma separated list, for pwrite() to a character device
 * (the MTD device) in the process that has it:
 *   flip:N    the first N writes store a byte flipped (a write the chip "lost")
 *   flipall   every write does (a chip that can't be written)
 *   eio:N     the first N writes fail with EIO
 *   slow:MS   every write takes MS milliseconds longer (time to kill the job)
 * Build: see test_pns_qemu.py (the A/B build's cross compiler). */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static long opt(const char *name)
{
    const char *e = getenv("PNS_FAULT"), *p;
    size_t n = strlen(name);
    for (p = e; p && *p; p = strchr(p, ',') ? strchr(p, ',') + 1 : NULL)
        if (!strncmp(p, name, n) && (p[n] == ':' || p[n] == ',' || !p[n]))
            return p[n] == ':' ? atol(p + n + 1) : 1;
    return 0;
}

ssize_t pwrite(int fd, const void *buf, size_t n, off_t off)
{
    static ssize_t (*real)(int, const void *, size_t, off_t);
    static long calls;
    struct stat st;
    long slow = opt("slow"), flip = opt("flip"), eio = opt("eio");
    if (!real)
        real = dlsym(RTLD_NEXT, "pwrite");
    if (n == 0 || fstat(fd, &st) || !S_ISCHR(st.st_mode))
        return real(fd, buf, n, off);
    calls++;
    if (slow > 0)
    {
        struct timespec t = { slow / 1000, (slow % 1000) * 1000000L };
        nanosleep(&t, NULL);
    }
    if (calls <= eio)
    {
        errno = EIO;
        return -1;
    }
    if (opt("flipall") || calls <= flip)
    {
        char *c = malloc(n);
        ssize_t r;
        memcpy(c, buf, n);
        c[n / 2] ^= 0x10;
        r = real(fd, c, n, off);
        free(c);
        return r;
    }
    return real(fd, buf, n, off);
}
