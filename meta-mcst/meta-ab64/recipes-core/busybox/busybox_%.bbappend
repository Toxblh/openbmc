# Without GNU coreutils (left out with the old updater) busybox provides
# head, od, dd...: the options the REIMU scripts use (gpio-funcs: head -c1)
FILESEXTRAPATHS_prepend := "${THISDIR}/${PN}:"
SRC_URI += "file://reimu-coreutils.cfg"
