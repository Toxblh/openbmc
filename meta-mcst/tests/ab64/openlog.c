/* LD_PRELOAD for test_regressions_qemu.py (bug 17): logs to OPENLOG
 * (default /tmp/openlog)
 *   - every open()/openat() of a /dev/mtd* node with its access mode:
 *     "path O_RDONLY|O_RDWR|O_WRONLY"
 *   - an fclose() of a FILE * that no fopen()/fdopen() of this process
 *     returned, or that was closed already: "fclose of an unknown FILE *"
 *     (not passed on: the call that ASan would report, without the crash
 *     that depends on what the stack held)
*/
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static void logit(const char *p, int fl)
{
    if (!p || !strstr(p, "mtd")) return;
    const char *f = getenv("OPENLOG");
    FILE *o = fopen(f ? f : "/tmp/openlog", "a");
    if (!o) return;
    int a = fl & O_ACCMODE;
    fprintf(o, "%s %s\n", p, a == O_RDONLY ? "O_RDONLY" : a == O_RDWR ? "O_RDWR" : "O_WRONLY");
    fclose(o);
}
static mode_t mode_of(int fl, va_list ap) { return (fl & O_CREAT) ? va_arg(ap, int) : 0; }
int open(const char *p, int fl, ...) { va_list ap; va_start(ap, fl); mode_t m = mode_of(fl, ap); va_end(ap);
    static int (*r)(const char *, int, ...); if (!r) r = dlsym(RTLD_NEXT, "open"); logit(p, fl); return r(p, fl, m); }
int open64(const char *p, int fl, ...) { va_list ap; va_start(ap, fl); mode_t m = mode_of(fl, ap); va_end(ap);
    static int (*r)(const char *, int, ...); if (!r) r = dlsym(RTLD_NEXT, "open64"); logit(p, fl); return r(p, fl, m); }
int openat(int d, const char *p, int fl, ...) { va_list ap; va_start(ap, fl); mode_t m = mode_of(fl, ap); va_end(ap);
    static int (*r)(int, const char *, int, ...); if (!r) r = dlsym(RTLD_NEXT, "openat"); logit(p, fl); return r(d, p, fl, m); }
int openat64(int d, const char *p, int fl, ...) { va_list ap; va_start(ap, fl); mode_t m = mode_of(fl, ap); va_end(ap);
    static int (*r)(int, const char *, int, ...); if (!r) r = dlsym(RTLD_NEXT, "openat64"); logit(p, fl); return r(d, p, fl, m); }

#define MAXF 256
static FILE *s_open[MAXF];
static void track(FILE *f)
{
    for (int i = 0; f && i < MAXF; i++) if (!s_open[i]) { s_open[i] = f; return; }
}
FILE *fopen(const char *p, const char *m) { static FILE *(*r)(const char *, const char *); if (!r) r = dlsym(RTLD_NEXT, "fopen");
    FILE *f = r(p, m); track(f); return f; }
FILE *fopen64(const char *p, const char *m) { static FILE *(*r)(const char *, const char *); if (!r) r = dlsym(RTLD_NEXT, "fopen64");
    FILE *f = r(p, m); track(f); return f; }
FILE *fdopen(int fd, const char *m) { static FILE *(*r)(int, const char *); if (!r) r = dlsym(RTLD_NEXT, "fdopen");
    FILE *f = r(fd, m); track(f); return f; }
int fclose(FILE *f)
{
    static int (*r)(FILE *); if (!r) r = dlsym(RTLD_NEXT, "fclose");
    if (f == stdin || f == stdout || f == stderr) return r(f);
    for (int i = 0; i < MAXF; i++) if (f && s_open[i] == f) { s_open[i] = NULL; return r(f); }
    const char *l = getenv("OPENLOG");
    int fd = open(l ? l : "/tmp/openlog", O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd >= 0) { char b[64]; int n = snprintf(b, sizeof b, "fclose of an unknown FILE * %p\n", (void *)f); write(fd, b, n); close(fd); }
    return 0;
}

