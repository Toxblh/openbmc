// zig's compiler-rt puts weak byte-loop memcpy/memmove/memset into every
// executable, and they win over glibc's tuned ARM versions. These strong
// definitions forward to glibc's (found once with dlsym(RTLD_NEXT)); until
// then (inside dlsym itself) a plain byte loop does the job.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stddef.h>
#include <stdint.h>

typedef void *(*cpy_fn)(void *, const void *, size_t);
typedef void *(*set_fn)(void *, int, size_t);
static cpy_fn libc_memcpy, libc_memmove;
static set_fn libc_memset;
static int resolving;

static void resolve(void)
{
    if (resolving)
        return;
    resolving = 1;
    cpy_fn c = (cpy_fn)dlsym(RTLD_NEXT, "memcpy");
    cpy_fn m = (cpy_fn)dlsym(RTLD_NEXT, "memmove");
    set_fn s = (set_fn)dlsym(RTLD_NEXT, "memset");
    libc_memmove = m;
    libc_memset = s;
    libc_memcpy = c;
}

static void *slow_move(void *d, const void *s, size_t n)
{
    unsigned char *dp = d;
    const unsigned char *sp = s;
    if (dp < sp || dp >= sp + n)
        while (n--) *dp++ = *sp++;
    else
        while (n--) dp[n] = sp[n];
    return d;
}

void *memcpy(void *d, const void *s, size_t n)
{
    if (__builtin_expect(!libc_memcpy, 0)) { resolve(); if (!libc_memcpy) return slow_move(d, s, n); }
    return libc_memcpy(d, s, n);
}

void *memmove(void *d, const void *s, size_t n)
{
    if (__builtin_expect(!libc_memmove, 0)) { resolve(); if (!libc_memmove) return slow_move(d, s, n); }
    return libc_memmove(d, s, n);
}

void *memset(void *d, int c, size_t n)
{
    if (__builtin_expect(!libc_memset, 0)) {
        resolve();
        if (!libc_memset) { unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
    }
    return libc_memset(d, c, n);
}

__attribute__((constructor(101))) static void fastmem_init(void) { resolve(); }
