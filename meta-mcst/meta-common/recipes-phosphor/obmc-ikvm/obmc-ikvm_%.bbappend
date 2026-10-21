FILESEXTRAPATHS_prepend := "${THISDIR}/files:"
SRC_URI += " file://0000-allow-connect-from-all-ip-addresses.patch file://0001-add-vnc-auth.patch "
# the mouse has a wheel (VNC buttons 4-7; reimu-kvm writes the same 7-byte report)
SRC_URI += " file://0002-mouse-wheel.patch "

# the video engine gives ~44 frames/s at 1920x1080; the default of 30 caps VNC lower
do_install_append() {
    sed -i "s|^ExecStart=/usr/bin/obmc-ikvm .*|& -f 60|" ${D}${systemd_system_unitdir}/start-ipkvm.service
}
