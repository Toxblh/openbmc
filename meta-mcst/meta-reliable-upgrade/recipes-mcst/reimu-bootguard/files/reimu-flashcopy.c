/*
 * reimu-flashcopy: copy the first <size> bytes of one flash chip to another,
 * erasing and writing only the erase blocks that differ, then verify.
 *
 *   reimu-flashcopy <src> <dst> <size>      copy, prints progress lines
 *   reimu-flashcopy --compare <a> <b> <size>
 *                                           exit 0 if identical, 1 if not
 *
 * <src>/<dst> are MTD character devices (/dev/mtdN: MEMERASE before write)
 * or regular files (tests). Progress on stdout:
 *   progress <percent> <changed blocks>
 *   done <changed blocks> <total blocks>
 *
 * Copyright (c) 2026 MCST REIMU contributors. Licensed under GPLv2.
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

struct dev
{
    const char *path;
    int fd;
    int mtd;          /* MTD char device: erase before write */
    unsigned erasesize;
    unsigned long long size;
};

static int open_dev(struct dev *d, const char *path, int rw)
{
    struct stat st;
    struct mtd_info_user mi;

    d->path = path;
    d->fd = open(path, (rw ? O_RDWR : O_RDONLY) | O_CLOEXEC);
    if (d->fd < 0 || fstat(d->fd, &st))
    {
        fprintf(stderr, "reimu-flashcopy: %s: %s\n", path, strerror(errno));
        return -1;
    }
    d->mtd = S_ISCHR(st.st_mode) && ioctl(d->fd, MEMGETINFO, &mi) == 0;
    if (d->mtd)
    {
        d->erasesize = mi.erasesize;
        d->size = mi.size;
    }
    else
    {
        d->erasesize = 4096;
        d->size = S_ISREG(st.st_mode) ? (unsigned long long)st.st_size : 0;
        if (S_ISBLK(st.st_mode))
        {
            off_t end = lseek(d->fd, 0, SEEK_END);
            d->size = end > 0 ? (unsigned long long)end : 0;
        }
    }
    return 0;
}

static int read_at(struct dev *d, void *buf, size_t n, unsigned long long off)
{
    size_t done = 0;
    while (done < n)
    {
        ssize_t r = pread(d->fd, (char *)buf + done, n - done, off + done);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0)
        {
            fprintf(stderr, "reimu-flashcopy: read %s at 0x%llx: %s\n", d->path, off + done, r ? strerror(errno) : "short read");
            return -1;
        }
        done += r;
    }
    return 0;
}

static int write_block(struct dev *d, const void *buf, size_t n, unsigned long long off)
{
    if (d->mtd)
    {
        struct erase_info_user ei = { .start = off, .length = n };
        if (ioctl(d->fd, MEMERASE, &ei))
        {
            fprintf(stderr, "reimu-flashcopy: erase %s at 0x%llx: %s\n", d->path, off, strerror(errno));
            return -1;
        }
    }
    size_t done = 0;
    while (done < n)
    {
        ssize_t r = pwrite(d->fd, (const char *)buf + done, n - done, off + done);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0)
        {
            fprintf(stderr, "reimu-flashcopy: write %s at 0x%llx: %s\n", d->path, off + done, r ? strerror(errno) : "short write");
            return -1;
        }
        done += r;
    }
    return 0;
}

int main(int argc, char **argv)
{
    int compare = argc == 5 && !strcmp(argv[1], "--compare");
    if (argc != 4 && !compare)
    {
        fprintf(stderr, "Usage: %s <src> <dst> <size>\n       %s --compare <a> <b> <size>\n", argv[0], argv[0]);
        return 2;
    }
    char **a = argv + compare;
    unsigned long long size = strtoull(a[3], NULL, 0);
    struct dev src, dst;
    if (open_dev(&src, a[1], 0) || open_dev(&dst, a[2], !compare)) return 2;
    if (size > src.size || size > dst.size)
    {
        fprintf(stderr, "reimu-flashcopy: %llu bytes don't fit (%s: %llu, %s: %llu)\n", size, src.path, src.size, dst.path, dst.size);
        return 2;
    }
    /* compare/write unit: 64 KiB, or the erase block when larger */
    size_t bs = 65536;
    if (dst.erasesize > bs) bs = dst.erasesize;
    if (bs % dst.erasesize || size % bs)
    {
        fprintf(stderr, "reimu-flashcopy: size must be a multiple of %zu (erase block %u)\n", bs, dst.erasesize);
        return 2;
    }
    char *s = malloc(bs), *d = malloc(bs);
    if (!s || !d) return 2;
    unsigned long long blocks = size / bs, changed = 0;
    int last = -1, rc = 0;

    for (unsigned long long i = 0; i < blocks; i++)
    {
        unsigned long long off = i * bs;
        if (read_at(&src, s, bs, off) || read_at(&dst, d, bs, off))
        {
            rc = 3;
            break;
        }
        if (memcmp(s, d, bs))
        {
            if (compare)
            {
                rc = 1;
                break;
            }
            if (write_block(&dst, s, bs, off) || read_at(&dst, d, bs, off))
            {
                rc = 3;
                break;
            }
            if (memcmp(s, d, bs))
            {
                fprintf(stderr, "reimu-flashcopy: verification failed at 0x%llx\n", off);
                rc = 3;
                break;
            }
            changed++;
        }
        int pct = (int)((i + 1) * 100 / blocks);
        if (!compare && pct != last && pct % 2 == 0)
        {
            printf("progress %d %llu\n", pct, changed);
            fflush(stdout);
            last = pct;
        }
    }
    free(s);
    free(d);
    if (rc) return rc;
    if (!compare)
    {
        if (fsync(dst.fd) && !dst.mtd)
        {
            fprintf(stderr, "reimu-flashcopy: fsync %s: %s\n", dst.path, strerror(errno));
            return 3;
        }
        printf("done %llu %llu\n", changed, blocks);
    }
    close(src.fd);
    close(dst.fd);
    return 0;
}
