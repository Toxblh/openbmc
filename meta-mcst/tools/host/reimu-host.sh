#!/bin/sh
# reimu-host.sh: the Elbrus host's side of the REIMU web KVM (1Э8СВ-uATX).
#
# The KVM page shows the host's screen in two ways, both set up here as system
# services (from boot on, on the login screen and in every session):
#   * JPEG from the BMC's video engine (BIOS, boot, any OS; up to ~40 frames/s).
#     It captures the ASPEED VGA, which X doesn't use when a real video card is
#     there: reimu-fbmirror copies the X screen into the ASPEED framebuffer
#     (only the changed rows, pointer drawn in).
#   * H.264 from the video card's own encoder (VAAPI), 60 frames/s at a few
#     Mbit/s: reimu-h264 takes the buffer on screen (KMS) without a copy through
#     the CPU, with the sound of the logged-in user (Opus); the BMC relays it
#     to the browser (Источник: «H.264 с хоста», button «Звук»).
# Also: an EDID for the ASPEED VGA, so it runs 1920x1080 without a monitor.
#
#   sudo ./reimu-host.sh install [--bmc IP] [--fps N] [--bitrate MBIT]
#                    IP: the BMC (only it may take the H.264 stream; default
#                    192.168.10.79); N: mirror updates/s (60); MBIT: H.264 (8)
#   sudo ./reimu-host.sh console     kernel messages and a login on the BMC
#                                    console (console=ttyS1, needs a reboot)
#   sudo ./reimu-host.sh uninstall   undo all of it (console= stays)
#        ./reimu-host.sh status
#
# Needs: gcc, libxcb headers (xcb shm damage xfixes), ffmpeg with VAAPI,
# python3. Files go to /usr/local/lib/reimu, units to /etc/systemd/system.
set -u
LIB=/usr/local/lib/reimu
UNITS="reimu-fbmirror.service reimu-h264.service"
XCONF=/etc/X11/xorg.conf.d/20-reimu-ast-off.conf
EDID_NAME=reimu-1920x1200.bin
FW=/lib/firmware/edid/$EDID_NAME
EDID_B64='AP///////wBJtSAZAQAAAAEkAQMONCB4Cu6Ro1RMmSYPUFQhCADRwLMAqUCBgJUAgQCBwHFAKDyAoHCwI0AwIDYACEUhAAAaAAAA/QAySx5QEQAKICAgICAgAAAA/ABSRUlNVSBLVk0KICAgAAAA/wBCTUMtVkdBCiAgICAgANo='
EDID_MD5=f2b7fecf2caa9adbf3b2952c0fc26818
HERE=$(cd "$(dirname "$0")" && pwd)

say() { printf '%s\n' "$*"; }
die() { printf 'reimu-host.sh: %s\n' "$*" >&2; exit 1; }
need_root() { [ "$(id -u)" = 0 ] || die "run with sudo"; }

# the ASPEED output (card0-VGA-1 on this board), found by driver
aspeed_conn() {
    for c in /sys/class/drm/card*-VGA-*; do
        [ "$(basename "$(readlink -f "$c/../device/driver")" 2>/dev/null)" = ast ] && { echo "$c"; return; }
    done
}
bootconf() { for f in /boot/boot.conf /boot.conf; do [ -f "$f" ] && { echo "$f"; return; }; done; }
# boot.conf: add or remove a kernel parameter on every cmdline= line
cmdline_param() { # add|del PARAM
    bc=$(bootconf)
    [ -n "$bc" ] && grep -q '^[[:space:]]*cmdline' "$bc" || { say "no boot.conf cmdline: $1 '$2' on the kernel command line yourself"; return; }
    case $1 in
        add) grep -q -- "$2" "$bc" && return
             cp -p "$bc" "$bc.reimu.$(date +%Y%m%d%H%M%S)"
             sed -i "/^[[:space:]]*cmdline/ s#\$# $2#" "$bc" ;;
        del) grep -q -- "$2" "$bc" || return
             cp -p "$bc" "$bc.reimu.$(date +%Y%m%d%H%M%S)"
             sed -i "s# $2##g" "$bc" ;;
    esac
    say "$bc: $1 $2"
}

# ------------------------------------------------------------------ run-mirror
# (the reimu-fbmirror service) the X server's display and cookie, whoever is
# logged in: the -auth file of the running X, tried on each X socket
run_mirror() {
    for pid in $(pgrep -x Xorg) $(pgrep -x X); do
        auth=$(tr '\0' '\n' < /proc/$pid/cmdline | sed -n '/^-auth$/{n;p;q}')
        for s in /tmp/.X11-unix/X*; do
            [ -S "$s" ] || continue
            if DISPLAY=:${s##*X} XAUTHORITY=$auth "$LIB/reimu-fbmirror" -t 2>/dev/null; then
                DISPLAY=:${s##*X} XAUTHORITY=$auth exec "$LIB/reimu-fbmirror" "$@"
            fi
        done
    done
    say "no X server yet"
    sleep 5
    exit 1
}

# ------------------------------------------------------------------ install
install_edid() {
    mkdir -p "${FW%/*}"
    echo "$EDID_B64" | base64 -d > "$FW.tmp"
    [ "$(md5sum "$FW.tmp" | cut -d' ' -f1)" = $EDID_MD5 ] || { rm -f "$FW.tmp"; die "EDID checksum mismatch"; }
    mv "$FW.tmp" "$FW" && chmod 644 "$FW"
    conn=$(aspeed_conn)
    [ -n "$conn" ] || { say "no ASPEED VGA connector found: EDID skipped"; return; }
    out=${conn##*/card[0-9]-}
    if [ -w /sys/module/drm/parameters/edid_firmware ]; then
        echo "$out:edid/$EDID_NAME" > /sys/module/drm/parameters/edid_firmware
        echo detect > "$conn/status" 2>/dev/null
    fi
    cmdline_param add "drm.edid_firmware=$out:edid/$EDID_NAME"
    say "EDID: ASPEED $out up to 1920x1080 without a monitor"
}

do_install() {
    need_root
    bmc=192.168.10.79 fps=60 bitrate=8
    while [ $# -gt 0 ]; do
        case $1 in
            --bmc) bmc=$2; shift ;;
            --fps) fps=$2; shift ;;
            --bitrate) bitrate=$2; shift ;;
            *) die "unknown option $1" ;;
        esac
        shift
    done
    for f in reimu-fbmirror.c reimu-h264.py; do [ -f "$HERE/$f" ] || die "$f must be next to $0"; done

    install_edid

    # X leaves the ASPEED card alone (it can't share buffers with the real card)
    mkdir -p "${XCONF%/*}"
    [ -f /etc/X11/xorg.conf.d/90-reimu-no-autogpu.conf ] || [ -f $XCONF ] ||
        printf '# REIMU: X leaves the ASPEED card (BMC VGA) alone, reimu-fbmirror copies the screen into it\nSection "ServerFlags"\n    Option "AutoAddGPU" "false"\nEndSection\n' > $XCONF

    mkdir -p $LIB
    gcc -O2 -o $LIB/reimu-fbmirror "$HERE/reimu-fbmirror.c" $(pkg-config --cflags --libs xcb xcb-shm xcb-damage xcb-xfixes) ||
        die "reimu-fbmirror: build failed (gcc and the libxcb headers are needed)"
    install -m 755 "$HERE/reimu-h264.py" "$0" $LIB/

    cat > /etc/systemd/system/reimu-fbmirror.service <<EOF
[Unit]
Description=REIMU KVM: the X screen into the ASPEED VGA (BMC video engine)
After=display-manager.service

[Service]
ExecStart=$LIB/reimu-host.sh run-mirror -f $fps
Restart=always
RestartSec=3

[Install]
WantedBy=graphical.target
EOF
    cat > /etc/systemd/system/reimu-h264.service <<EOF
[Unit]
Description=REIMU KVM: H.264 of the screen from the video card for the BMC $bmc
After=network.target

[Service]
ExecStart=/usr/bin/python3 $LIB/reimu-h264.py --allow $bmc --bitrate $bitrate
Restart=always
RestartSec=3
# /run/user stays visible: the sound comes from the logged-in user's PipeWire
# (ffmpeg for it runs as that user); its home must stay readable for that
InaccessiblePaths=-/root
ProtectSystem=strict
PrivateTmp=yes
# Mesa's shader cache (the home directory is hidden from the service)
CacheDirectory=reimu
Environment=XDG_CACHE_HOME=/var/cache/reimu

[Install]
WantedBy=multi-user.target
EOF
    # the per-user way of before (reimu.sh / reimu-2.sh)
    if [ -n "${SUDO_USER:-}" ]; then
        home=$(getent passwd "$SUDO_USER" | cut -d: -f6)
        rm -f "$home/.config/autostart/reimu-kvm.desktop"
        systemctl --user -M "$SUDO_USER@" stop reimu-fbmirror reimu-h264 2>/dev/null
    fi
    pkill -f "f fbdev /dev/fb" 2>/dev/null

    sync  # a sudden reset (the BMC rebooting resets this host) left empty files
    systemctl daemon-reload
    systemctl enable --now $UNITS
    sleep 2
    do_status
}

do_uninstall() {
    need_root
    systemctl disable --now $UNITS 2>/dev/null
    for u in $UNITS; do rm -f /etc/systemd/system/$u; done
    systemctl daemon-reload
    rm -rf $LIB $XCONF
    conn=$(aspeed_conn)
    out=${conn##*/card[0-9]-}
    [ -n "$out" ] && cmdline_param del "drm.edid_firmware=$out:edid/$EDID_NAME"
    echo > /sys/module/drm/parameters/edid_firmware 2>/dev/null
    rm -f "$FW"
    say "removed (X picks up the ASPEED card again after the next login)"
}

do_console() {
    need_root
    bc=$(bootconf)
    if [ -n "$bc" ] && grep -q '^[[:space:]]*cmdline' "$bc"; then
        cp -p "$bc" "$bc.reimu.$(date +%Y%m%d%H%M%S)"
        sed -i '/^[[:space:]]*cmdline/ s#console=ttyS0\([,[:space:]]\)#console=ttyS1\1#g; /^[[:space:]]*cmdline/ s#console=ttyS0$#console=ttyS1#' "$bc"
        grep '^[[:space:]]*cmdline' "$bc" | grep -q 'console=ttyS1' || sed -i "/^[[:space:]]*cmdline/ s#\$# console=ttyS1,115200#" "$bc"
        say "$bc: console=ttyS1 (after a reboot)"
    else
        say "no boot.conf cmdline: add console=ttyS1,115200 to the kernel command line yourself"
    fi
    systemctl enable --now serial-getty@ttyS1.service && say "login on ttyS1 (the BMC console)"
}

do_status() {
    for u in $UNITS; do
        printf '%-24s %s\n' "$u" "$(systemctl is-active $u 2>/dev/null)"
        journalctl -u $u -n 2 --no-pager -o cat 2>/dev/null | sed 's/^/    /'
    done
    conn=$(aspeed_conn)
    [ -n "$conn" ] && say "ASPEED VGA modes: $(head -3 "$conn/modes" 2>/dev/null | tr '\n' ' ')"
    other=$(pgrep -af "f fbdev /dev/fb" | head -1)
    [ -n "$other" ] && say "another mirror (ffmpeg) writes the framebuffer too: $other"
}

case "${1:-}" in
    install) shift; do_install "$@" ;;
    uninstall) do_uninstall ;;
    console) do_console ;;
    status) do_status ;;
    run-mirror) shift; run_mirror "$@" ;;
    *) sed -n '2,/^set -u/{/^set -u/d;s/^# \{0,1\}//;p}' "$0"; exit 2 ;;
esac
