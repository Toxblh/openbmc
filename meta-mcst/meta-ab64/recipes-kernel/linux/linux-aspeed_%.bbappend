# openbmc/linux dev-6.18 instead of dev-5.10: the whole config in
# reimu-5564/defconfig (the machine directory, ahead of meta-aspeed's aspeed-g5/)
# (the 5.10 fragments and patches of the other layers don't apply)
FILESEXTRAPATHS_prepend := "${THISDIR}/${PN}:"
KBRANCH = "dev-6.18"
LINUX_VERSION = "6.18.57"
SRCREV = "0168b08a4c77fb172f2edc68551d8c16e511ecd4"
SRC_URI = "git://github.com/openbmc/linux;protocol=https;branch=${KBRANCH} \
           file://defconfig \
           file://aspeed-bmc-mcst-reimu-5564.dts \
           file://reimu-ab64.dtsi \
          "
# a gadget connecting while the host has suspended the virtual hub: the
# remote wakeup as an edge each time (only the first one reached the host?)
SRC_URI += "file://0001-usb-gadget-aspeed-vhub-remote-wakeup-an-edge-each-time.patch"
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
