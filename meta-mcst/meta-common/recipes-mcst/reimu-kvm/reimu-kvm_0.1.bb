SUMMARY = "KVM straight to the browser"
DESCRIPTION = "Pushes the ASPEED video engine's JPEG frames (or the host's H.264 from reimu-h264) to the web UI over its own TLS websocket (port 8443) and writes keyboard/mouse to the USB HID gadget; D-Bus xyz.openbmc_project.Reimu.Kvm hands out the connection tickets"
SECTION = "base"
PR = "r2"
LICENSE = "GPLv2"
LIC_FILES_CHKSUM = "file://LICENSE;md5=b234ee4d69f5fce4486a80fdaf4a4263"

inherit systemd
inherit features_check

REQUIRED_DISTRO_FEATURES = "systemd"
DEPENDS = "systemd openssl"
RDEPENDS_${PN} += "obmc-ikvm"
SYSTEMD_SERVICE_${PN} = "reimu-kvm.service reimu-hid-watch.service"

SRC_URI = " \
            file://LICENSE \
            file://Makefile \
            file://reimu-kvm.c \
            file://reimu-kvm.service \
            file://xyz.openbmc_project.Reimu.Kvm.conf \
            file://reimu-hid-watch \
            file://reimu-hid-watch.service \
          "

S = "${WORKDIR}"

do_install() {
  install -d ${D}/usr/sbin ${D}${systemd_system_unitdir} ${D}${sysconfdir}/dbus-1/system.d
  install -m 755 reimu-kvm reimu-hid-watch ${D}/usr/sbin
  install -m 644 reimu-kvm.service reimu-hid-watch.service ${D}${systemd_system_unitdir}
  install -m 644 xyz.openbmc_project.Reimu.Kvm.conf ${D}${sysconfdir}/dbus-1/system.d
}

FILES_${PN} = "/usr/sbin ${systemd_system_unitdir} ${sysconfdir}/dbus-1"
