# REIMU A/B: the slot's rofs is checked and copied to RAM before it's mounted
FILESEXTRAPATHS_prepend := "${THISDIR}/files:"
SRC_URI += "file://reimu-ab-rofs"
do_install_append() {
    install -m 0644 ${WORKDIR}/reimu-ab-rofs ${D}/reimu-ab-rofs
    # sourced after the options are read (debug_takeover, findmtd, $optfile)
    sed -i '/^echo rofs = /i . /reimu-ab-rofs' ${D}/init
    grep -q '^\. /reimu-ab-rofs' ${D}/init
}
FILES_${PN} += " /reimu-ab-rofs "
