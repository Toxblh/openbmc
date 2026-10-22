/* LD_PRELOAD for test_reimu_flash.py: the flash "loses" writes. FLAKY_BAD=N:
 * the first N pwrite calls store a flipped byte; FLAKY_BAD=-1: every one. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

ssize_t pwrite(int fd, const void *buf, size_t n, off_t off)
{
    static ssize_t (*real)(int, const void *, size_t, off_t);
    static int calls;
    const char *e = getenv("FLAKY_BAD");
    int bad = e ? atoi(e) : 0;
    if (!real)
        real = dlsym(RTLD_NEXT, "pwrite");
    if (n && (bad < 0 || calls++ < bad))
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
