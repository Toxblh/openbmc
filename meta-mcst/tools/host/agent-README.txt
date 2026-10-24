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

Files / файлы: install.sh (checks the SHA-256 of the files below / проверяет
SHA-256 файлов), reimu-hostd.py, reimu-hostd.service, reimu-hostd.conf,
PAIRING.TXT (the one-time pairing string, valid 15 min / одноразовая строка
привязки, действует 15 минут; the BMC removes this disk after the agent
connects / BMC отключает этот диск, когда агент подключится).
