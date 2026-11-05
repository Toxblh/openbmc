#!/bin/sh
# run-usbnet.sh WORKDIR: the USB network to the host (reimu-usbnet) in the HID
# test VM (build-hidvm.sh; started here if it isn't running), the VM as both
# ends (vm/usbnetscen.py). The script from the build container's working
# tree; REV=<git rev>: at that revision (red runs).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
W=$(cd "$1" && pwd)
REPO=${REPO:-/work/openbmc}
REV=${REV:-}
S=$W/share/un
rm -rf "$S" && mkdir -p "$S"
F=meta-mcst/meta-common/recipes-mcst/reimu-usbnet/files/reimu-usbnet
if [ -n "$REV" ]; then docker exec reimu git -c safe.directory=* -C $REPO show "$REV:$F" > "$S/reimu-usbnet" 2>/dev/null
else docker exec reimu cat "$REPO/$F" > "$S/reimu-usbnet"; fi
cp -R "$HERE/vm/." "$W/share/"
"$HERE/vssh" "$W" true 2>/dev/null || "$HERE/run-hidvm.sh" "$W" || exit 1
"$HERE/vssh" "$W" "cd /mnt && python3 usbnetscen.py /mnt/un"
