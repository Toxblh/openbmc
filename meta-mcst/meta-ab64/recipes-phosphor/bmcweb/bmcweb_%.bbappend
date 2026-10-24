# NetworkProtocol SSH: OpenSSH's sshd.socket (it looked for dropbear: false);
# EventLog ClearLog: success without an rsyslog to reload (was 500)
FILESEXTRAPATHS_prepend := "${THISDIR}/${PN}:"
SRC_URI += " file://0001-NetworkProtocol-SSH-from-sshd.socket-ClearLog-without-rsyslog.patch "
# Chassis Sensors/Thermal/Power: all of D-Bus's sensors (one board, its
# sensors come and go with the platform power), not 404
SRC_URI += " file://0002-Chassis-sensors-every-sensor-without-an-all_sensors-association.patch "
