SUMMARY = "REIMU host link: the BMC's end of the host agent"
DESCRIPTION = "reimu-hostlink: TLS link (IPv6 link-local first) with the host agent reimu-hostd: the host's inventory and health on D-Bus (xyz.openbmc_project.Reimu.HostLink, Sensor.Value objects for the GPU and disks), events, the GPU fan policy and an allow-list of requests to the host"
SECTION = "base"
PR = "r1"
LICENSE = "GPLv2"
LIC_FILES_CHKSUM = "file://LICENSE;md5=b234ee4d69f5fce4486a80fdaf4a4263"

inherit systemd
inherit features_check

REQUIRED_DISTRO_FEATURES = "systemd"
DEPENDS = "systemd openssl"
SYSTEMD_SERVICE_${PN} = "reimu-hostlink.service"
# the installer it hands the host: /agent/ over HTTPS, the USB disk (AgentDisk)
RDEPENDS_${PN} += "reimu-agent-bundle"

SRC_URI = " \
            file://LICENSE \
            file://Makefile \
            file://hostlink.h \
            file://hl-dbus.h \
            file://reimu-hostlink.c \
            file://hl-proto.c \
            file://hl-json.c \
            file://hl-net.c \
            file://hl-dbus.c \
            file://reimu-hostlink.service \
            file://reimu-hostlink.conf \
            file://xyz.openbmc_project.Reimu.HostLink.conf \
          "

S = "${WORKDIR}"

do_install() {
  install -d ${D}/usr/sbin ${D}${systemd_system_unitdir} ${D}${sysconfdir}/dbus-1/system.d
  install -m 755 reimu-hostlink ${D}/usr/sbin
  install -m 644 reimu-hostlink.service ${D}${systemd_system_unitdir}
  install -m 644 reimu-hostlink.conf ${D}${sysconfdir}
  install -m 644 xyz.openbmc_project.Reimu.HostLink.conf ${D}${sysconfdir}/dbus-1/system.d
}

FILES_${PN} = "/usr/sbin ${systemd_system_unitdir} ${sysconfdir}"
CONFFILES_${PN} = "${sysconfdir}/reimu-hostlink.conf"
