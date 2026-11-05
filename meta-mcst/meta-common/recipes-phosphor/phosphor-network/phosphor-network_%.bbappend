FILESEXTRAPATHS_prepend := "${THISDIR}:"
SRC_URI += " file://0001-enable-Domains-parameter.patch "
# the USB network to the host (reimu-usbnet: usb0) is not the BMC's LAN: no
# D-Bus object (Redfish, IPMI, the web UI's network settings), no
# 00-bmc-usb0.network with a DHCP client on it
SRC_URI += " file://0002-ignore-interfaces-of-IGNORED_INTERFACES.patch "

EXTRA_OECONF += " --disable-link-local-autoconfiguration "

do_install_append() {
    d=${D}${systemd_system_unitdir}/xyz.openbmc_project.Network.service.d
    install -d $d
    printf '%s\n' '# phosphor-network_%.bbappend (REIMU): reimu-usbnet owns usb0' '[Service]' \
        'Environment=IGNORED_INTERFACES=usb0' > $d/reimu-ignore.conf
}
FILES_${PN} += "${systemd_system_unitdir}/xyz.openbmc_project.Network.service.d"
