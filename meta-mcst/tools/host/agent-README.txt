REIMU host agent (reimu-hostd) / Агент хоста REIMU
==================================================

The agent tells the BMC what the host is and how it is doing (CPU, memory and
ECC, PCIe, disks, network, the video card and its fan) and lets the BMC set
the GPU fan policy and ask for a clean shutdown/reboot. It talks to the BMC
over the board's internal LAN (IPv6 link-local: no cable, no DHCP needed),
TLS with the BMC's certificate pinned.

Агент сообщает BMC состав и состояние хоста (процессор, память и ECC, PCIe,
диски, сеть, видеокарта и её вентилятор), BMC может задать режим вентилятора
видеокарты и попросить корректно выключить/перезагрузить ОС. Связь — по
внутренней сети платы (IPv6 link-local: без кабеля и DHCP), TLS с
закреплённым сертификатом BMC.

Once paired the BMC is the only master of the I2C buses it shares with the
host: pwmd is disabled and the host's fan drivers kept out. With the BMC down
or on old firmware nobody drives the fans (they hold their last duty, 100 % if
reimu-fand --failsafe set it); removing the agent gives the host its control
back (after a reboot of the host).

После привязки шинами I²C, общими с хостом, управляет только BMC: pwmd отключён,
драйверы вентиляторов хоста не загружаются. При выключенном BMC или старой
прошивке вентиляторами никто не управляет (держат последнее значение, 100 %
после reimu-fand --failsafe); удаление агента возвращает управление хосту
(после его перезагрузки).

With a video card besides the ASPEED (systemd, ffmpeg with VAAPI) the agent
also sets up the picture of the BMC's web KVM: reimu-h264 (the screen as H.264,
or H.265 where the browser decodes it, from the video card, 60 frames/s) and reimu-fbmirror (the X screen into the
ASPEED VGA for the BMC's own JPEG; built here, needs gcc and the libxcb
headers, only when the video card is the boot VGA). Every install or update
of the agent updates and restarts them; the BMC learns their state from the
agent (the KVM page says why when there is no H.264).

С видеокартой помимо ASPEED (systemd, ffmpeg с VAAPI) агент настраивает и
картинку веб-KVM BMC: reimu-h264 (экран в H.264, или в H.265, если браузер
его декодирует, видеокартой, 60 к/с) и
reimu-fbmirror (экран X в VGA ASPEED для JPEG самого BMC; собирается здесь,
нужны gcc и заголовки libxcb, только если видеокарта — загрузочная VGA).
Каждая установка или обновление агента обновляет и перезапускает их; BMC
узнаёт их состояние от агента (страница KVM скажет, почему нет H.264).

Install / Установка (root, python3 required / нужен python3):

  From this disk / с этого диска (the pairing string is in PAIRING.TXT):
    sudo sh /run/media/$USER/REIMU-AGENT/install.sh
    (no desktop / без графики:
       sudo mount -o ro /dev/disk/by-label/REIMU-AGENT /mnt && sudo sh /mnt/install.sh)

  From the BMC / с BMC: the command on the BMC's web page «Хост» →
  «Подключить агент хоста» (curl … | sudo sh -s -- --pair 'reimu1:…').

  Again with a new pairing string / снова с новой строкой привязки:
    sudo sh install.sh --pair 'reimu1:…'
  Remove / удалить:
    sudo sh install.sh --uninstall [--purge]

After install / после установки:
  systemctl status reimu-hostd ; journalctl -u reimu-hostd
  settings / настройки: /etc/reimu-hostd.conf

No systemd (SysV init) / без systemd: install.sh sees it and installs
/etc/init.d/reimu-hostd, on at boot by update-rc.d, chkconfig, insserv or rc?.d
links (it says so if none of them is there / скажет, если ничего этого нет):
  /etc/init.d/reimu-hostd status ; tail /var/log/reimu-hostd.log
python3 3.6 or newer / python3 версии 3.6 или новее.

Files / файлы: install.sh (checks the SHA-256 of the files below / проверяет
SHA-256 файлов), reimu-hostd.py, reimu-hostd.service, reimu-hostd.conf,
reimu-hostd.init, reimu-h264.py, reimu-host.sh, reimu-fbmirror.c (the
picture services / службы картинки KVM),
PAIRING.TXT (the one-time pairing string, valid 15 min / одноразовая строка
привязки, действует 15 минут; the BMC removes this disk after the agent
connects / BMC отключает этот диск, когда агент подключится).
