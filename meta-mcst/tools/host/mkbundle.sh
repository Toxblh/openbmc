#!/bin/sh
# mkbundle.sh OUT [--disk]: the host agent as the BMC serves it (the recipe
# reimu-agent-bundle runs it at build time; tests/test_install_sh.py too):
#   OUT/agent/      install.sh (the SHA-256 of the files written into it),
#                   reimu-hostd.py, reimu-hostd.service, reimu-hostd.conf,
#                   SHA256SUMS, README.txt         -> /usr/share/www/agent/
#   --disk: OUT/disk.img  a FAT12 image «REIMU-AGENT» (1 MiB) with the same
#                   files and PAIRING.TXT, 512 bytes of placeholder: the BMC's
#                   reimu-agent-disk writes the pairing string over them at
#                   OUT/disk.offset (bytes) when the page asks for the disk
# Needs sha256sum, awk; --disk: mkfs.vfat (dosfstools), mcopy (mtools), grep.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$1
DISK=${2:-}
FILES="reimu-hostd.py reimu-hostd.service reimu-hostd.conf"
A=$OUT/agent
mkdir -p "$A"
for f in $FILES; do
    cp "$HERE/$f" "$A/$f"
    chmod 0644 "$A/$f"
done
(cd "$A" && sha256sum $FILES) > "$A/SHA256SUMS"
awk -v sums="$A/SHA256SUMS" '$0 == "@SHA256SUMS@" { while ((getline l < sums) > 0) print l; next } { print }' \
    "$HERE/install.sh" > "$A/install.sh"
grep -q '@SHA256SUMS@' "$A/install.sh" && { echo "mkbundle.sh: the hashes not written into install.sh" >&2; exit 1; }
cp "$HERE/agent-README.txt" "$A/README.txt"
chmod 0644 "$A/install.sh" "$A/SHA256SUMS" "$A/README.txt"

[ "$DISK" = --disk ] || exit 0
IMG=$OUT/disk.img
MARK=REIMU-PAIRING-PLACEHOLDER
rm -f "$IMG"
# a fixed volume id: the same image from the same files
mkfs.vfat -C -n REIMU-AGENT -i 52454d55 "$IMG" 1024 >/dev/null
P=$OUT/PAIRING.TXT
{ printf '%s' $MARK; printf '%486s' ''; printf '\n'; } > "$P"
[ "$(wc -c < "$P")" -eq 512 ]
export MTOOLS_SKIP_CHECK=1
# PAIRING.TXT first: the first data cluster, one sector of it
mcopy -i "$IMG" "$P" ::PAIRING.TXT
for f in install.sh README.txt $FILES; do mcopy -i "$IMG" "$A/$f" "::$f"; done
rm -f "$P"
n=$(LC_ALL=C grep -c -a $MARK "$IMG" || true)
[ "$n" = 1 ] || { echo "mkbundle.sh: the placeholder found $n times in the image" >&2; exit 1; }
LC_ALL=C grep -obUa $MARK "$IMG" | cut -d: -f1 > "$OUT/disk.offset"
