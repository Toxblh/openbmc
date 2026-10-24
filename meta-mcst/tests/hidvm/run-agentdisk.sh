#!/bin/sh
# run-agentdisk.sh WORKDIR: the installer's USB disk (reimu-agent-disk) in the
# HID test VM (build-hidvm.sh; started here if it isn't running), the VM as
# the USB host (vm/agentdiskscen.py). The pieces from the build container's
# working tree: mkbundle.sh's disk image, the script; REV=<git rev>: the
# script at that revision (red runs).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
W=$(cd "$1" && pwd)
REPO=${REPO:-/work/openbmc}
REV=${REV:-}
S=$W/share/ad
rm -rf "$S" && mkdir -p "$S"
docker exec reimu sh -c "rm -rf /tmp/ad-bundle && sh $REPO/meta-mcst/tools/host/mkbundle.sh /tmp/ad-bundle --disk" || exit 1
docker exec reimu tar -C /tmp/ad-bundle -cf - . | tar -C "$S" -xf -
F=meta-mcst/meta-common/recipes-mcst/reimu-agent-bundle/files/reimu-agent-disk
if [ -n "$REV" ]; then docker exec reimu git -c safe.directory=* -C $REPO show "$REV:$F" > "$S/reimu-agent-disk" 2>/dev/null
else docker exec reimu cat "$REPO/$F" > "$S/reimu-agent-disk"; fi
cp -R "$HERE/vm/." "$W/share/"
"$HERE/vssh" "$W" true 2>/dev/null || "$HERE/run-hidvm.sh" "$W" || exit 1
"$HERE/vssh" "$W" "cd /mnt && python3 agentdiskscen.py /mnt/ad"
