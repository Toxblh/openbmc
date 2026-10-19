SUMMARY = "REIMU web UI"
DESCRIPTION = "Web interface of REIMU BMC: KVM (noVNC over bmcweb /kvm/0), serial consoles with timestamped history, read-write virtual media, sensors, inventory and host boot firmware (PNS) information"
SECTION = "base"
PR = "r1"
# REIMU code is GPLv2, bundled noVNC is MPL-2.0, xterm.js is MIT
LICENSE = "GPLv2 & MPL-2.0 & MIT"
LIC_FILES_CHKSUM = "file://LICENSE;md5=b234ee4d69f5fce4486a80fdaf4a4263 \
                    file://www/vendor/novnc/LICENSE.txt;md5=e7708e98cb73559ec64e5abed9aecfb0 \
                    file://www/vendor/xterm/LICENSE;md5=a6f7d231a2745fd8aaa465e2dd7c7c04 \
                   "

inherit allarch

SRC_URI = "file://LICENSE file://www"

S = "${WORKDIR}"

do_install() {
    install -d ${D}${datadir}/www ${D}${docdir}/${BPN}
    cp -r ${S}/www/. ${D}${datadir}/www/
    # licenses are documentation, not web content
    mv ${D}${datadir}/www/vendor/novnc/LICENSE.txt ${D}${docdir}/${BPN}/LICENSE.noVNC
    mv ${D}${datadir}/www/vendor/xterm/LICENSE ${D}${docdir}/${BPN}/LICENSE.xterm
    # bmcweb serves foo.js.gz as foo.js with Content-Encoding: gzip
    find ${D}${datadir}/www -type f \( -name '*.js' -o -name '*.css' -o -name '*.html' -o -name '*.svg' \) -exec gzip -9 -n {} \;
    find ${D}${datadir}/www -type f -exec chmod 0644 {} +
    find ${D}${datadir}/www -type d -exec chmod 0755 {} +
}

FILES_${PN} = "${datadir}/www ${docdir}/${BPN}"
