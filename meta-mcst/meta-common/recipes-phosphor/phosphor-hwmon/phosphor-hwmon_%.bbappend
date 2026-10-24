inherit meson

EXTRA_OEMESON += "-Dtests=disabled"
# Both are booleans without a default in meson_options.txt, i.e. true: a
# sensor that fails to read was published with -errno as its value (-5 for
# EIO). Instead the error codes in a device's REMOVERCS (hwmon-instantiator
# lists the I2C ones) take the sensor off D-Bus until it reads again.
# update-functional-on-fail stays off too: with it a sensor whose read fails
# is put back on D-Bus at once as 0 with Functional false, every poll. Any
# other error exits phosphor-hwmon: restarted (see the drop-in), slowly
# enough never to reach the start limit
EXTRA_OEMESON += "-Dnegative-errno-on-fail=false -Dupdate-functional-on-fail=false"
SRC_URI += " file://hwmon-restart.conf "
do_install_append() {
    install -d ${D}${systemd_system_unitdir}/xyz.openbmc_project.Hwmon@.service.d
    install -m 644 ${WORKDIR}/hwmon-restart.conf ${D}${systemd_system_unitdir}/xyz.openbmc_project.Hwmon@.service.d/restart.conf
}
FILES_${PN} += "${systemd_system_unitdir}/xyz.openbmc_project.Hwmon@.service.d"

PACKAGECONFIG[max31785-msl] = "-Denable-max31785-msl=true, -Denable-max31785-msl=false"

SRCREV = "9c4053455f479c85221712f790187076669e0d59"

FILESEXTRAPATHS_prepend := "${THISDIR}:"
SRC_URI += " file://0001-separate-hi-and-lo-alarms.patch "
