KMACHINE = "aspeed"
KERNEL_DEVICETREE = "${KMACHINE}-bmc-mcst-${MACHINE}.dtb"

require conf/machine/include/obmc-bsp-common.inc
require conf/machine/include/flash.inc
require conf/machine/include/serial.inc

PREFERRED_PROVIDER_virtual/phosphor-led-manager-config-native = "reimu-phosphor-led-manager-config-native"
require conf/machine/include/reimu-fetch-fixes.inc
