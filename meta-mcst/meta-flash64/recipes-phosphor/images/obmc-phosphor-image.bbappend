OBMC_KERNEL_MODULES_64M = " \
  kernel-module-ip6table-filter \
  kernel-module-ip6table-mangle \
  kernel-module-ip6-tables \
  kernel-module-ip6t-ipv6header \
  kernel-module-ip6t-reject \
  kernel-module-iptable-filter \
  kernel-module-iptable-mangle \
  kernel-module-iptable-nat \
  kernel-module-ip-tables \
  kernel-module-ipt-reject \
  kernel-module-x-tables \
  kernel-module-xt-addrtype \
  kernel-module-xt-conntrack \
  kernel-module-xt-log \
  kernel-module-xt-mark \
  kernel-module-xt-masquerade \
  kernel-module-xt-nat \
  kernel-module-xt-nflog \
  kernel-module-xt-state \
  kernel-module-xt-tcpmss \
  kernel-module-xt-tcpudp \
  "


OBMC_IMAGE_EXTRA_INSTALL_append += " \
  ${OBMC_KERNEL_MODULES_64M} \
  perf \
  tcpdump \
  ethtool \
  phosphor-webui \
  ipmitool \
  iptables \
  obmc-phosphor-buttons-signals \
  obmc-phosphor-buttons-handler \
  reimu-power-control \
  vncpasswd \
  sudo \
  hwmon-instantiator \
  phosphor-sel-logger \
  rtcread \
  reimu-webui \
  reimu-pns \
  reimu-kvm \
  "

# Platform COM ports are owned by reimu-serial, which also serves the
# obmc-console socket (bmcweb /console0), so obmc-console-server must not
# grab the same UART.
IMAGE_FEATURES_remove = "obmc-console"
