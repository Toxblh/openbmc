/*
 * reimu-fit: take the kernel, device tree and initramfs out of a FIT image
 * (U-Boot's fitImage, a flattened device tree), for kexec.
 *
 *   reimu-fit FIT DIR      writes DIR/kernel, DIR/fdt, DIR/ramdisk
 *
 * FIT may be an MTD partition: only the FIT's own size is read.
 */

#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FDT_MAGIC 0xd00dfeed

static uint32_t be(const unsigned char *p) { return ntohl(*(const uint32_t *)p); }

int main(int argc, char **argv)
{
    unsigned char hdr[40], *fit;
    uint32_t total, ostruct, ostrings, p;
    int depth = 0, inimages = 0, found = 0;
    const unsigned char *data = NULL, *type = NULL;
    uint32_t dlen = 0;
    FILE *f;

    if (argc != 3)
    {
        fprintf(stderr, "usage: reimu-fit FIT DIR\n");
        return 2;
    }
    if (!(f = fopen(argv[1], "rb")) || fread(hdr, 1, sizeof hdr, f) != sizeof hdr ||
        be(hdr) != FDT_MAGIC)
    {
        fprintf(stderr, "reimu-fit: %s: not a FIT image\n", argv[1]);
        return 1;
    }
    total = be(hdr + 4), ostruct = be(hdr + 8), ostrings = be(hdr + 12);
    if (total < sizeof hdr || total > (64u << 20) || !(fit = malloc(total)))
        return fprintf(stderr, "reimu-fit: bad size %u\n", total), 1;
    memcpy(fit, hdr, sizeof hdr);
    if (fread(fit + sizeof hdr, 1, total - sizeof hdr, f) != total - sizeof hdr)
        return fprintf(stderr, "reimu-fit: %s: short read\n", argv[1]), 1;
    fclose(f);

    for (p = ostruct; p + 4 <= total;)
    {
        uint32_t tok = be(fit + p);
        p += 4;
        if (tok == 1) /* BEGIN_NODE */
        {
            const char *name = (const char *)fit + p;
            depth++;
            if (depth == 2)
                inimages = !strcmp(name, "images");
            if (depth == 3 && inimages)
                data = type = NULL;
            p += (strlen(name) + 4) & ~3u;
        }
        else if (tok == 2) /* END_NODE: an image under /images is complete */
        {
            if (depth == 3 && inimages && data && type)
            {
                const char *out = !strcmp((const char *)type, "kernel") ? "kernel" :
                                  !strcmp((const char *)type, "flat_dt") ? "fdt" :
                                  !strcmp((const char *)type, "ramdisk") ? "ramdisk" : NULL;
                if (out)
                {
                    char path[4096];
                    snprintf(path, sizeof path, "%s/%s", argv[2], out);
                    if (!(f = fopen(path, "wb")) || fwrite(data, 1, dlen, f) != dlen || fclose(f))
                        return perror(path), 1;
                    printf("%s %u\n", out, dlen);
                    found++;
                }
            }
            depth--;
        }
        else if (tok == 3) /* PROP */
        {
            uint32_t len = be(fit + p), nameoff = be(fit + p + 4);
            const char *name = (const char *)fit + ostrings + nameoff;
            if (depth == 3 && inimages && !strcmp(name, "data"))
                data = fit + p + 8, dlen = len;
            if (depth == 3 && inimages && !strcmp(name, "type"))
                type = fit + p + 8;
            p += 8 + ((len + 3) & ~3u);
        }
        else if (tok == 4) /* NOP */
            ;
        else
            break; /* END */
    }
    if (found < 2)
        return fprintf(stderr, "reimu-fit: %s: no kernel/fdt in it\n", argv[1]), 1;
    return 0;
}
