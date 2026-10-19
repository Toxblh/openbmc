#!/bin/sh
# Starts the new REIMU web UI (devkit) on a REIMU BMC without touching its flash.
#
#   ./reimu-webdev-run.sh IP [LOGIN [PASSWORD]]        upload and start
#   ./reimu-webdev-run.sh IP [LOGIN [PASSWORD]] --stop    back to the stock web
#   ./reimu-webdev-run.sh IP [LOGIN [PASSWORD]] --status  what is running
#
# LOGIN/PASSWORD default to root / 0penBmc. Everything goes to the BMC's /tmp
# (RAM): a BMC reboot also returns it to the stock firmware.
# Needs: ssh (OpenSSH 8.4+ to pass the password; older ones ask for it).
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
KIT="$HERE/reimu-webdev.tar.gz"

action=start
args=
for a in "$@"; do
    case "$a" in
        --stop) action=stop ;;
        --status) action=status ;;
        -h|--help) sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        -*) echo "unknown option $a" >&2; exit 2 ;;
        *) args="$args $a" ;;
    esac
done
set -- $args
[ $# -ge 1 ] || { sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
IP=$1
USER_=${2:-root}
PASS=${3:-0penBmc}

[ -f "$KIT" ] || { echo "no $KIT next to this script" >&2; exit 1; }

TMP=$(mktemp -d)
trap 'ssh -o ControlPath="$TMP/c" -O exit x 2>/dev/null || true; rm -rf "$TMP"' EXIT INT TERM
printf '#!/bin/sh\necho "$REIMU_PASS"\n' > "$TMP/askpass"
chmod 700 "$TMP/askpass"

# one SSH connection for all steps: the password is sent once
bmc() {
    REIMU_PASS="$PASS" SSH_ASKPASS="$TMP/askpass" SSH_ASKPASS_REQUIRE=force DISPLAY=${DISPLAY:-x} \
    ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR \
        -o ConnectTimeout=15 -o ControlMaster=auto -o ControlPath="$TMP/c" -o ControlPersist=120 \
        -o PreferredAuthentications=password,keyboard-interactive,publickey \
        "$USER_@$IP" "$@"
}

echo "== $IP: connecting as $USER_"
bmc 'test -x /usr/bin/bmcweb && grep -q . /etc/os-release' ||
    { echo "$IP doesn't look like an OpenBMC/REIMU BMC (no /usr/bin/bmcweb)" >&2; exit 1; }
bmc '. /etc/os-release; echo "   firmware: ${PRETTY_NAME:-?} ${VERSION_ID:-}"; echo "   uptime: $(uptime)"'

case $action in
status)
    bmc 'if [ -x /tmp/reimu-webdev/reimu-webdev ]; then /tmp/reimu-webdev/reimu-webdev status; else echo "   devkit is not on this BMC (stock web)"; fi'
    exit 0 ;;
stop)
    bmc 'if [ -x /tmp/reimu-webdev/reimu-webdev ]; then /tmp/reimu-webdev/reimu-webdev stop && rm -rf /tmp/reimu-webdev; else echo "   devkit is not running"; fi'
    echo "== stock web is back: https://$IP/"
    exit 0 ;;
esac

echo "== uploading $(wc -c < "$KIT" | tr -d ' ') bytes to /tmp (RAM)"
bmc 'f=$(df -k /tmp | awk "NR==2{print \$4}"); [ "${f:-0}" -gt 20000 ] || { echo "not enough free space in /tmp: ${f} KiB" >&2; exit 1; }'
bmc 'cat > /tmp/reimu-webdev.tar.gz' < "$KIT"

echo "== starting"
bmc 'set -e
    if [ -x /tmp/reimu-webdev/reimu-webdev ] && /tmp/reimu-webdev/reimu-webdev status >/dev/null 2>&1; then
        echo "   an older devkit is running: stopping it"
        /tmp/reimu-webdev/reimu-webdev stop >/dev/null 2>&1 || true
    fi
    rm -rf /tmp/reimu-webdev && tar xzf /tmp/reimu-webdev.tar.gz -C /tmp && rm -f /tmp/reimu-webdev.tar.gz
    /tmp/reimu-webdev/reimu-webdev start'

code=$(curl -sk -o /dev/null -m 15 -w '%{http_code}' "https://$IP/" 2>/dev/null || echo "?")
echo
echo "== done: https://$IP/  (login $USER_; certificate warning is expected)  HTTP $code"
echo "   back to the stock web: $0 $IP${2:+ $2}${3:+ ***} --stop   (or reboot the BMC)"
