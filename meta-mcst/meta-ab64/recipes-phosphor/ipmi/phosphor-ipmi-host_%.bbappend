# netipmid/ipmid missed a user table rewritten in the same second (jffs2
# mtime): a user just configured over IPMI was refused ("invalid role")
FILESEXTRAPATHS_prepend := "${THISDIR}/${PN}:"
SRC_URI += " file://0001-user-channel-data-reload-when-the-file-was-replaced.patch "
# mc info: Firmware Revision 1.02 for "reimu-1.0.2-..." (was 0.00); mc guid
# answers the machine-id GUID (was "Unspecified error")
SRC_URI += " file://0002-Get-Device-ID-firmware-revision-Get-System-GUID.patch "
