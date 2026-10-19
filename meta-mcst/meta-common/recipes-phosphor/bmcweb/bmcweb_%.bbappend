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
EXTRA_OEMESON_append = " -Dreimu-serial-socket=enabled -Dvm-websocket=enabled"
