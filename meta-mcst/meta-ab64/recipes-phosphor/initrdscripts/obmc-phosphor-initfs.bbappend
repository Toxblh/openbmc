# REIMU A/B: the slot's rofs is checked and copied to RAM before it's mounted
FILESEXTRAPATHS_prepend := "${THISDIR}/files:"
SRC_URI += "file://reimu-ab-rofs"
do_install_append() {
    install -m 0644 ${WORKDIR}/reimu-ab-rofs ${D}/reimu-ab-rofs
    # sourced after the options are read (debug_takeover, findmtd, $optfile)
    sed -i '/^echo rofs = /i . /reimu-ab-rofs' ${D}/init
    grep -q '^\. /reimu-ab-rofs' ${D}/init
    # the shutdown flasher of the static layout writes /run/initramfs/image-X
    # to the partition named X: on A/B only reimu-ab writes the BMC flash
    # (image-bmc would overwrite both slots, U-Boot and its environment)
    sed -i '/^imglist=\$(echo \$image\*)/i for f in $image*; do test -e "$f" || continue; echoerr "A/B firmware: ${f##*/} not written, BMC images are installed with reimu-ab"; rm -f "$f"; done' ${D}/update
    grep -q 'A/B firmware: ' ${D}/update
}
FILES_${PN} += " /reimu-ab-rofs "
