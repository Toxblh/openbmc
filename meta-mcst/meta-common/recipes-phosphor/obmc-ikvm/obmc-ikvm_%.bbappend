FILESEXTRAPATHS_prepend := "${THISDIR}/files:"
SRC_URI += " file://0000-allow-connect-from-all-ip-addresses.patch file://0001-add-vnc-auth.patch "
# the mouse has a wheel (VNC buttons 4-7; reimu-kvm writes the same 7-byte report)
SRC_URI += " file://0002-mouse-wheel.patch "
# the HID writer: never blocks on the keyboard, releases what a client held
# when it goes, no Shift "wakeup" without a client (see the patch)
SRC_URI += " file://0003-HID-nonblocking-release-no-stray-shift.patch "
# the gadget script in place of obmc-ikvm's: each report descriptor in one
# write() to configfs, read back and compared (its echo -ne of the mouse
# descriptor, which has a 0x0a byte, came out of bash in two writes and
# configfs kept the last 14 bytes: no mouse on the host)
SRC_URI += " file://create_usbhid.sh "

# the video engine gives ~44 frames/s at 1920x1080; the default of 30 caps VNC lower
do_install_append() {
    sed -i "s|^ExecStart=/usr/bin/obmc-ikvm .*|& -f 60|" ${D}${systemd_system_unitdir}/start-ipkvm.service
    install -m 0755 ${WORKDIR}/create_usbhid.sh ${D}${bindir}/create_usbhid.sh
}
