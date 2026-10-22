/*
 * reimu-flash: write an image into the BMC flash and prove it got there.
 *
 *   reimu-flash write  DEV OFFSET FILE [TRIES]   erase and write the erase
 *        blocks that differ, read the whole range back and compare; on a
 *        mismatch write again, up to TRIES (3) times. Exit 0 only when the
 *        flash holds FILE byte for byte.
 *   reimu-flash verify DEV OFFSET FILE           exit 0 if it does, 1 if not
 *
 * DEV is an MTD character device (/dev/mtdN, a partition or the whole chip)
 * or a regular file (tests). OFFSET (bytes, 0x.. allowed) must be on an
 * erase block; FILE is padded with 0xff to the next erase block. U-Boot
 * (the first 0xe0000 bytes of the chip, the "u-boot" partition) is never
 * written: that is the one thing a bad write here couldn't recover from.
 * Progress on stdout: "write <done%>", "verify <done%>", "ok <changed blocks>".
 */

#include <errno.h>
#include <fcntl.h>
#include <mtd/mtd-user.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define UBOOT_END 0xe0000UL

static const char *me = "reimu-flash";

static int fail(const char *what, const char *path)
{
    fprintf(stderr, "%s: %s: %s\n", me, path, what ? what : strerror(errno));
    return -1;
}

/* the "name" of /dev/mtdN in sysfs ("" for files) */
static void mtd_name(const char *dev, char *name, size_t len)
{
    char p[96];
    FILE *f;
    const char *n = strrchr(dev, '/');

    name[0] = 0;
    snprintf(p, sizeof p, "/sys/class/mtd/%s/name", n ? n + 1 : dev);
    if ((f = fopen(p, "r")))
    {
        if (fgets(name, len, f))
            name[strcspn(name, "\n")] = 0;
        fclose(f);
    }
}

static int pread_all(int fd, void *b, size_t n, off_t off)
{
    size_t got = 0;
    while (got < n)
    {
        ssize_t r = pread(fd, (char *)b + got, n - got, off + got);
        if (r <= 0)
            return -1;
        got += r;
    }
    return 0;
}

static int pwrite_all(int fd, const void *b, size_t n, off_t off)
{
    size_t put = 0;
    while (put < n)
    {
        ssize_t r = pwrite(fd, (const char *)b + put, n - put, off + put);
        if (r <= 0)
            return -1;
        put += r;
    }
    return 0;
}

/* blocks of the device that differ from img: 1 per block in diff[] */
static long compare(int fd, const char *dev, const unsigned char *img,
                    size_t len, unsigned long off, unsigned es, char *diff,
                    const char *label)
{
    unsigned char *b = malloc(es);
    long n = 0;
    size_t i, nb = len / es;

    if (!b)
        return fail("out of memory", dev);
    for (i = 0; i < nb; i++)
    {
        if (pread_all(fd, b, es, off + i * es))
        {
            free(b);
            return fail(NULL, dev);
        }
        diff[i] = memcmp(b, img + i * es, es) != 0;
        n += diff[i];
        if (label && (i % 256 == 0 || i + 1 == nb))
            printf("%s %zu%%\n", label, (i + 1) * 100 / nb), fflush(stdout);
    }
    free(b);
    return n;
}

int main(int argc, char **argv)
{
    int write_mode, fd, ifd, tries = 3, mtd;
    unsigned long off;
    unsigned es = 4096;
    unsigned long long devsize;
    struct stat st;
    struct mtd_info_user mi;
    unsigned char *img;
    char *diff, name[64];
    size_t len, nb, i;
    long changed = 0, bad;

    if (argc < 5 || (strcmp(argv[1], "write") && strcmp(argv[1], "verify")))
    {
        fprintf(stderr, "usage: %s write|verify DEV OFFSET FILE [TRIES]\n", me);
        return 2;
    }
    write_mode = !strcmp(argv[1], "write");
    off = strtoul(argv[3], NULL, 0);
    if (argc > 5)
        tries = atoi(argv[5]);

    fd = open(argv[2], (write_mode ? O_RDWR : O_RDONLY) | O_SYNC);
    if (fd < 0 || fstat(fd, &st))
        return fail(NULL, argv[2]), 1;
    mtd = S_ISCHR(st.st_mode) && ioctl(fd, MEMGETINFO, &mi) == 0;
    if (mtd)
        es = mi.erasesize, devsize = mi.size;
    else
        devsize = st.st_size;

    mtd_name(argv[2], name, sizeof name);
    if (write_mode && (!strcmp(name, "u-boot") ||
                       (!strcmp(name, "bmc") && off < UBOOT_END)))
    {
        fprintf(stderr, "%s: refusing to write U-Boot (%s at 0x%lx)\n",
                me, argv[2], off);
        return 1;
    }

    ifd = open(argv[4], O_RDONLY);
    if (ifd < 0 || fstat(ifd, &st))
        return fail(NULL, argv[4]), 1;
    len = (st.st_size + es - 1) / es * es;
    if (off % es || off + len > devsize || !st.st_size)
    {
        fprintf(stderr, "%s: %s (%lld bytes) doesn't fit %s at 0x%lx "
                "(size 0x%llx, erase block 0x%x)\n", me, argv[4],
                (long long)st.st_size, argv[2], off, devsize, es);
        return 1;
    }
    img = malloc(len);
    nb = len / es;
    diff = malloc(nb);
    if (!img || !diff)
        return fail("out of memory", argv[4]), 1;
    memset(img, 0xff, len);
    if (pread_all(ifd, img, st.st_size, 0))
        return fail(NULL, argv[4]), 1;
    close(ifd);

    bad = compare(fd, argv[2], img, len, off, es, diff, write_mode ? NULL : "verify");
    if (bad < 0)
        return 1;
    if (!write_mode)
    {
        printf(bad ? "differ %ld\n" : "ok 0\n", bad);
        return bad ? 1 : 0;
    }

    while (bad > 0 && tries-- > 0)
    {
        /* erase runs of differing blocks at once: the SPI NOR driver then
         * uses its larger (64 KiB) erase where it can */
        for (i = 0; i < nb;)
        {
            size_t j = i;
            if (!diff[i])
            {
                i++;
                continue;
            }
            while (j < nb && diff[j])
                j++;
            if (mtd)
            {
                struct erase_info_user e = { off + i * es, (j - i) * es };
                if (ioctl(fd, MEMERASE, &e))
                    return fail(NULL, argv[2]), 1;
            }
            for (; i < j; i++)
            {
                /* an erased flash block needs no write (a file does) */
                size_t k = 0;
                while (mtd && k < es && img[i * es + k] == 0xff)
                    k++;
                if (k < es && pwrite_all(fd, img + i * es, es, off + i * es))
                    return fail(NULL, argv[2]), 1;
                changed++;
                if (changed % 256 == 0)
                    printf("write %zu%%\n", (i + 1) * 100 / nb), fflush(stdout);
            }
        }
        /* read back everything, not only what was written */
        bad = compare(fd, argv[2], img, len, off, es, diff, "verify");
        if (bad < 0)
            return 1;
        if (bad)
            fprintf(stderr, "%s: %ld blocks differ after writing, %s\n", me,
                    bad, tries > 0 ? "writing them again" : "giving up");
    }
    if (bad)
        return 1;
    printf("ok %ld\n", changed);
    return 0;
}
