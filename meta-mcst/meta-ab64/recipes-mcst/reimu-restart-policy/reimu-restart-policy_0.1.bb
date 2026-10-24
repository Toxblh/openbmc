SUMMARY = "REIMU: restart policy of the BMC's long-running services"
DESCRIPTION = "Drop-ins that keep the daemons the BMC needs running 24/7: \
restarted 5 s after any exit, without the start limit. OpenBMC's default \
(phosphor-systemd-policy: 2 starts in 30 s, RestartSec=1s) leaves a daemon \
that crashed twice within 30 s failed until the next BMC reboot - and a BMC \
reboot resets the host on this board."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"
inherit allarch

# A drop-in never sets what the unit's own file declares: a unit with its
# own RestartSec= or StartLimit*= is not in REIMU_RESTART_ALWAYS (the units
# there declare at most Restart=always, the same). Units that declare their
# whole policy are in none of the lists: reimu-kvm (RestartSec=10: waits for
# bmcweb's certificate), reimu-pnsd.
#
# Restart=always, RestartSec=5, no start limit. Socket-activated ones
# (bmcweb, phosphor-ipmi-net@, busybox-syslog, triggerhappy) keep their
# socket in PID 1 the whole time; the service is started again on it.
# Templates (name@.service) cover every instance.
REIMU_RESTART_ALWAYS = " \
    bmcweb.service \
    phosphor-ipmi-host.service \
    phosphor-ipmi-net@.service \
    xyz.openbmc_project.State.Host.service \
    xyz.openbmc_project.State.Chassis.service \
    xyz.openbmc_project.State.BMC.service \
    start-ipkvm.service \
    xyz.openbmc_project.User.Manager.service \
    xyz.openbmc_project.Network.service \
    phosphor-certificate-manager@.service \
    xyz.openbmc_project.Settings.service \
    xyz.openbmc_project.Logging.service \
    xyz.openbmc_project.Logging.IPMI.service \
    xyz.openbmc_project.Inventory.Manager.service \
    xyz.openbmc_project.Time.Manager.service \
    xyz.openbmc_project.LED.GroupManager.service \
    xyz.openbmc_project.led.controller@.service \
    phosphor-button-handler.service \
    obmc-fru-fault-monitor.service \
    phosphor-systemd-target-monitor.service \
    xyz.openbmc_project.FruDevice.service \
    rngd.service \
    busybox-syslog.service \
    busybox-klogd.service \
    triggerhappy.service \
"
# their own Restart= stays (on-failure), only the limit and the delay change:
# upstream's RestartSec=0 without a start limit would restart it in a loop
REIMU_RESTART_NOLIMIT = "systemd-networkd.service"
# only no start limit: units with their own Restart= and RestartSec=
# (reimupwr 2 s, reimu-serial 2 s, ObjectMapper 5 s, Chassis.Buttons 3 s),
# and oneshots restarted along with a service they Require (tmux-logger with
# reimu-serial: the third restart of reimu-serial within 30 s left the serial
# console's tmux failed)
REIMU_STARTLIMIT_ONLY = " \
    reimupwr.service \
    reimu-serial.service \
    xyz.openbmc_project.ObjectMapper.service \
    xyz.openbmc_project.Chassis.Buttons.service \
    tmux-logger.service \
"

do_install() {
    for u in ${REIMU_RESTART_ALWAYS}; do
        install -d ${D}${systemd_system_unitdir}/$u.d
        printf '%s\n' '# reimu-restart-policy: back 5 s after any exit, no start limit (24/7)' \
            '[Unit]' 'StartLimitIntervalSec=0' '[Service]' 'Restart=always' 'RestartSec=5' \
            > ${D}${systemd_system_unitdir}/$u.d/reimu-restart.conf
    done
    for u in ${REIMU_RESTART_NOLIMIT}; do
        install -d ${D}${systemd_system_unitdir}/$u.d
        printf '%s\n' '# reimu-restart-policy: no start limit (24/7)' \
            '[Unit]' 'StartLimitIntervalSec=0' '[Service]' 'RestartSec=5' \
            > ${D}${systemd_system_unitdir}/$u.d/reimu-restart.conf
    done
    for u in ${REIMU_STARTLIMIT_ONLY}; do
        install -d ${D}${systemd_system_unitdir}/$u.d
        printf '%s\n' '# reimu-restart-policy: no start limit (the rest of the policy: the unit)' \
            '[Unit]' 'StartLimitIntervalSec=0' \
            > ${D}${systemd_system_unitdir}/$u.d/reimu-restart.conf
    done
}
FILES_${PN} = "${systemd_system_unitdir}"
