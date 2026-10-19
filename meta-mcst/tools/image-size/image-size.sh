#!/bin/bash
# Where the space in the rootfs goes, and what removing packages would save.
# Runs in the build container after a build with INHERIT += "buildhistory".
#
#   image-size.sh BUILD_DIR [PLAN] [MACHINE]
#
# The squashfs is taken from the deployed .mtd.tar, each file is compressed
# like squashfs does (xz, 128 KiB blocks) and attributed to its package
# (pkgdata); PLAN (default plan-reimu-5564.txt) lists cumulative removal steps,
# each also drops whatever only it needed (RDEPENDS from buildhistory).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
B=$(cd "$1" && pwd); PLAN=${2:-$HERE/plan-reimu-5564.txt}; M=${3:-reimu-5564}
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
tar xf "$(readlink -f "$B/tmp/deploy/images/$M/obmc-phosphor-image-$M.static.mtd.tar")" -C "$W" image-rofs image-kernel
unsquashfs -q -d "$W/rofs" "$W/image-rofs" >/dev/null 2>&1 || true
python3 "$HERE/xzsizes.py" "$W/rofs" "$W/sizes.tsv"
real=$(stat -c %s "$W/image-rofs")
scale=$(awk -F'\t' -v r="$real" '{c += $3} END {printf "%.4f", r / c}' "$W/sizes.tsv")
echo "kernel (fitImage + initramfs): $(($(stat -c %s "$W/image-kernel") / 1024)) KiB, rofs: $((real / 1024)) KiB"
python3 "$HERE/plan.py" "$B/buildhistory/images/${M//-/_}/glibc/obmc-phosphor-image" \
    "$B/tmp/pkgdata/$M" "$W/sizes.tsv" "$scale" "$PLAN"
