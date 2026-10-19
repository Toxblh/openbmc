# fatdir: a folder as a FAT32 flash drive

Tests of `reimu-webui/files/www/js/lib/fatdir.js` with node (20+), dosfstools
and mtools:

    node test-build.mjs DIR vol.img              # build the volume of DIR
    dd if=vol.img of=part.img bs=512 skip=2048   # the partition
    fsck.vfat -n part.img                        # must be clean
    mcopy -s -n -m -i part.img ::/ out/ && diff -r DIR out

    # what the host changed: modify a copy of part.img with mtools
    # (mcopy/mdel/mmd), put the MBR back in front, then
    node test-changes.mjs DIR vol.img vol-modified.img
