FILESEXTRAPATHS_prepend := "${THISDIR}/files:"

SRC_URI += "file://reimu_state_hook file://config.json \
            file://0001-nbd-proxy-disable-nbd-client-netlink-support.patch \
           "

NBD_PROXY_CONFIG_JSON = "${WORKDIR}/config.json"

# Runs after every do_install_append (including the aspeed-layer one that
# installs the stock read-only hook), so this hook always wins.
do_install[postfuncs] += "reimu_install_state_hook"
reimu_install_state_hook() {
    install -d ${D}${sysconfdir}/nbd-proxy/
    install -m 0755 ${WORKDIR}/reimu_state_hook ${D}${sysconfdir}/nbd-proxy/state
}

FILES_${PN} += "${sysconfdir}/nbd-proxy/state"
