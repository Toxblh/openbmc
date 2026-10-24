SUMMARY = "A support library for REIMU software"
DESCRIPTION = "A support library for REIMU software"
SECTION = "base"
PR = "r1"
LICENSE = "GPLv2"
LIC_FILES_CHKSUM = "file://LICENSE;md5=b234ee4d69f5fce4486a80fdaf4a4263"

inherit meson

SRC_URI = "git://github.com/makise-homura/libreimu.git;protocol=https \
           file://0001-reimu_fdt-free-the-previous-device-tree.patch \
           file://0002-reimu_find_filename-copy-the-name-before-closedir.patch \
           file://0003-reimu_readfile-without-a-size-read-errors-and-the-end.patch \
          "
# pinned: a build must not change by itself
SRCREV = "4b706bc6d73e3c384cc995547be7cacbb2069a8a"

DEPENDS = "dtc dbus libgpiod"

S = "${WORKDIR}/git"
