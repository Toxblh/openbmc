FILESEXTRAPATHS_prepend := "${THISDIR}:"
SRC_URI += " file://0001-create-logging-directory.patch file://0001-fix-power-commands.patch "

# REIMU web UI support:
#  - /reimu/serial/<N> websocket to reimu-serial (consoles with history);
#  - /vm/0/1 (USB CD-ROM) next to /vm/0/0; NBD replies are buffered
#    dynamically, the fixed 128 KiB buffer made bmcweb abort with
#    std::length_error on pipelined reads (fatal for read-write media);
#  - larger timer queue: a burst of requests made bmcweb drop connections;
#  - CSP allows inline styles (xterm.js) and data: images (noVNC cursor).
SRC_URI += " file://0002-reimu-serial-websocket-vm-rw-csp.patch "
# firmware upload: wait up to 120 s (not 10) for the updater to accept the image
SRC_URI += " file://0003-update-service-wait-for-image.patch "
# firmware images (static 64 MiB layout, ~45 MB tar) are uploaded through
# Redfish UpdateService in one request
EXTRA_OEMESON_append = " -Dreimu-serial-socket=enabled -Dvm-websocket=enabled -Dhttp-body-limit=64"
# KVM: no Nagle on the websocket and VNC sockets, ChaCha20 preferred (no AES
# instructions on the ARM11)
SRC_URI += " file://0004-kvm-nodelay-chacha20.patch "
