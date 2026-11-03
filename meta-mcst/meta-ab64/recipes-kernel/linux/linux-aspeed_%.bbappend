# Linux 7.3 stable from kernel.org instead of openbmc/linux dev-6.18 (an
# experiment): none of the openbmc tree's own commits is used by this board
# (its drivers are all upstream). The whole config in reimu-5564/defconfig
# (the machine directory, ahead of meta-aspeed's aspeed-g5/), a minimal one
# (make savedefconfig of 7.3).
FILESEXTRAPATHS_prepend := "${THISDIR}/${PN}:"
LINUX_VERSION = "7.3.1"
PV = "${LINUX_VERSION}"
# a tarball (kernel-yocto makes it a git tree with one commit, branch master):
# no SRCREV to check, no openbmc/linux clone on every CI build
SRCREV = "INVALID"
KBRANCH = "master"
LINUX_VERSION_EXTENSION = "-reimu"
SRC_URI = "https://cdn.kernel.org/pub/linux/kernel/v7.x/linux-${LINUX_VERSION}.tar.xz;name=kernel \
           file://defconfig \
           file://aspeed-bmc-mcst-reimu-5564.dts \
           file://reimu-ab64.dtsi \
          "
SRC_URI[kernel.sha256sum] = "fa595aad93734f829c963c2a90ef7c3673224fd7fdae7b49406e397cde2de231"
S = "${WORKDIR}/linux-${LINUX_VERSION}"
# (no vhub remote-wakeup patch: making the manual remote wakeup an edge each
# time did not wake the host's suspended hub on the board, 2026-10-25; the
# host keeps the hub awake instead: power/control "on" for 1d6b:0107)
# virtual media: DVD-sized ISOs (Windows, Linux installers) as a CD-ROM, not
# cut at 2.2 GiB
SRC_URI += "file://0002-usb-gadget-f_mass_storage-CD-ROM-images-bigger-than-2.2-GiB.patch"
# QEMU's mx66l51235f (the A/B tests' BMC flash) has an SFDP that mainline's
# Macronix table without the fixed fields can't parse ("BFPT parsing
# failed"): the revert openbmc/linux dev-6.18 carries (61e0f969ac7d). The
# board's flashes are Micron n25q512ax3, not affected.
SRC_URI += "file://0003-Revert-mtd-spi-nor-macronix-Drop-the-redundant-flash-info-fields.patch"
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

# the kconfiglib of this Yocto's kern-tools can't parse 6.18+'s Kconfig
do_kernel_configcheck[noexec] = "1"

# 6.18+'s modules_install makes no "source" link, which kernel_do_install of
# this Yocto removes
do_install_prepend() {
    install -d ${D}${nonarch_base_libdir}/modules/${KERNEL_VERSION}
    ln -sfn /dev/null ${D}${nonarch_base_libdir}/modules/${KERNEL_VERSION}/source
}
