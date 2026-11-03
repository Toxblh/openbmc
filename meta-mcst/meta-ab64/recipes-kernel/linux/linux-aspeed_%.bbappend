# openbmc/linux dev-6.18 instead of dev-5.10: the whole config in
# reimu-5564/defconfig (the machine directory, ahead of meta-aspeed's aspeed-g5/)
# (the 5.10 fragments and patches of the other layers don't apply)
FILESEXTRAPATHS_prepend := "${THISDIR}/${PN}:"
KBRANCH = "dev-6.18"
LINUX_VERSION = "6.18.60"
SRCREV = "4a2f1ea25e7a0704492712c41e3048b0bbfe1332"
SRC_URI = "git://github.com/openbmc/linux;protocol=https;branch=${KBRANCH} \
           file://defconfig \
           file://aspeed-bmc-mcst-reimu-5564.dts \
           file://reimu-ab64.dtsi \
          "
# (no vhub remote-wakeup patch: making the manual remote wakeup an edge each
# time did not wake the host's suspended hub on the board, 2026-10-25; the
# host keeps the hub awake instead: power/control "on" for 1d6b:0107)
# virtual media: DVD-sized ISOs (Windows, Linux installers) as a CD-ROM, not
# cut at 2.2 GiB
SRC_URI += "file://0002-usb-gadget-f_mass_storage-CD-ROM-images-bigger-than-2.2-GiB.patch"
LIC_FILES_CHKSUM = "file://COPYING;md5=6bc538ed5bd9a7fc9398086aedcd7e46"

do_add_mcst_files () {
    cp ${WORKDIR}/aspeed-bmc-mcst-reimu-5564.dts ${WORKDIR}/reimu-ab64.dtsi \
       ${STAGING_KERNEL_DIR}/arch/arm/boot/dts/aspeed/
    grep -q mcst-reimu-5564 ${STAGING_KERNEL_DIR}/arch/arm/boot/dts/aspeed/Makefile ||
        echo 'dtb-$(CONFIG_ARCH_ASPEED) += aspeed-bmc-mcst-reimu-5564.dtb' >> \
            ${STAGING_KERNEL_DIR}/arch/arm/boot/dts/aspeed/Makefile
    # no "+" of a tree that isn't a clean tag: make-mod-scripts prepares the tree
    # for out-of-tree modules with no LOCALVERSION set, they would get RELEASE+
    # and not load
    sed -i 's/^elif \[ "${LOCALVERSION+set}" != "set" \]; then$/elif false; then/' ${STAGING_KERNEL_DIR}/scripts/setlocalversion
    grep -q '^elif false; then$' ${STAGING_KERNEL_DIR}/scripts/setlocalversion ||
        bbfatal "setlocalversion: the plus sign not removed"
}

# A fixed release. obmc-phosphor-kernel-version puts the commit of the patched
# tree in it, and that commit is new each time the patches are applied: with a
# cache the kernel can be rebuilt (a new initramfs) while its modules come from
# the cache, for another release, and then no module loads. Set here, after
# do_kernel_configme, with the auto suffix off (deleted there, it defaults to y).
do_configure_prepend() {
    sed -i '/CONFIG_LOCALVERSION/d' ${B}/.config
    echo 'CONFIG_LOCALVERSION="-reimu"' >> ${B}/.config
    echo '# CONFIG_LOCALVERSION_AUTO is not set' >> ${B}/.config
}

# no copy of the kernel in the rootfs' /boot (it is in the kernel partition)
RDEPENDS_${KERNEL_PACKAGE_NAME}-base = ""

# the kconfiglib of this Yocto's kern-tools can't parse 6.18's Kconfig
do_kernel_configcheck[noexec] = "1"

# 6.18's modules_install makes no "source" link, which kernel_do_install of
# this Yocto removes
do_install_prepend() {
    install -d ${D}${nonarch_base_libdir}/modules/${KERNEL_VERSION}
    ln -sfn /dev/null ${D}${nonarch_base_libdir}/modules/${KERNEL_VERSION}/source
}
