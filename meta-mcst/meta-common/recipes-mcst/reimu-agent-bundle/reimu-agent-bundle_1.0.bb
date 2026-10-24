SUMMARY = "REIMU host agent bundle: the installer the BMC hands its host"
DESCRIPTION = "The host agent reimu-hostd (meta-mcst/tools/host) as the BMC serves it: /usr/share/www/agent/ over bmcweb's HTTPS (install.sh with the SHA-256 of the agent's files written in, the files, SHA256SUMS, README.txt), and a FAT image «REIMU-AGENT» with the same files that reimu-agent-disk shows the host as a read-only USB disk with the pairing string on it (reimu-hostlink's AgentDisk)"
SECTION = "base"
PR = "r1"
LICENSE = "GPLv2"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/GPL-2.0;md5=801f80980d171dd6425610833a22dbe6"

inherit allarch

# mkfs.vfat, mcopy: the disk image is made here, only PAIRING.TXT's sector
# is written on the BMC
DEPENDS = "dosfstools-native mtools-native"

# the agent's own files live with the host's tools
FILESEXTRAPATHS_prepend := "${THISDIR}/../../../tools/host:${THISDIR}/files:"
SRC_URI = " \
            file://reimu-hostd.py \
            file://reimu-hostd.service \
            file://reimu-hostd.conf \
            file://install.sh \
            file://mkbundle.sh \
            file://agent-README.txt \
            file://reimu-agent-disk \
          "

S = "${WORKDIR}"
B = "${WORKDIR}/build"

do_compile() {
    rm -rf ${B}/out
    sh ${S}/mkbundle.sh ${B}/out --disk
}

do_install() {
    install -d ${D}${datadir}/www/agent ${D}${datadir}/reimu-agent ${D}${libexecdir}
    # served as they are (bmcweb would send a .gz as Content-Encoding: gzip:
    # curl | sh would get gzip)
    install -m 0644 ${B}/out/agent/* ${D}${datadir}/www/agent/
    install -m 0644 ${B}/out/disk.img ${B}/out/disk.offset ${D}${datadir}/reimu-agent/
    install -m 0755 ${S}/reimu-agent-disk ${D}${libexecdir}/
}

FILES_${PN} = "${datadir}/www/agent ${datadir}/reimu-agent ${libexecdir}/reimu-agent-disk"
