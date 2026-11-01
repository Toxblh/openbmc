#!/bin/sh
# backup-stock.sh: read-only check of a BMC on the stock REIMU 1.0.2 firmware
# and a dump of its whole flash, split for "reimu-ab restore-stock".
#
#   backup-stock.sh BMC [DIR]     (default DIR: ./backup-<date>)
#
# BMC: [user@]host[:port]. SSHPASS set: password login through SSH_ASKPASS.
# Stops if the BMC isn't stock 1.0.2 or its U-Boot isn't the tested one.
set -eu
UBOOT_SHA=1ef70f586108d7e486d11993938001c2dd1529652c2108f3a99edbf0534e2eb0
bmc=${1:?BMC}; dir=${2:-./backup-$(date +%Y%m%d-%H%M%S)}
host=${bmc%:*}; port=22; [ "$host" != "$bmc" ] && port=${bmc##*:}
case $host in *@*) ;; *) host=root@$host ;; esac
SSHO="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=20"
if [ -n "${SSHPASS:-}" ]; then
    ap=$(mktemp); trap 'rm -f "$ap"' EXIT
    printf '#!/bin/sh\necho "%s"\n' "$SSHPASS" > "$ap"; chmod +x "$ap"
    export SSH_ASKPASS=$ap SSH_ASKPASS_REQUIRE=force DISPLAY=x
fi
r() { ssh -p "$port" $SSHO "$host" "$@"; }
sha() { if command -v sha256sum >/dev/null; then sha256sum "$@"; else shasum -a 256 "$@"; fi; }
stop() { echo "STOP: $*" >&2; exit 1; }

ver=$(r 'sed -n s/^VERSION_ID=//p /etc/os-release')
[ "$ver" = reimu-1.0.2 ] || stop "firmware is '$ver', not the stock reimu-1.0.2"
echo "firmware: $ver"
r 'grep -q "\"u-boot\"" /proc/mtd && grep -q "\"rofs\"" /proc/mtd' || stop "not the stock flash layout"
[ "$(r 'grep -c . /proc/mtd')" -ge 6 ] || stop "not the stock flash layout"
r 'fw_printenv ethaddr eth1addr'

mkdir -p "$dir"
echo "reading the flash (64 MiB, a few minutes)..."
r 'sync; cat /dev/mtd0' > "$dir/flash.bin"
[ "$(wc -c < "$dir/flash.bin" | tr -d ' ')" = 67108864 ] || stop "flash.bin isn't 64 MiB"
# everything up to the rwfs must match; the rwfs is mounted and may change
here=$(dd if="$dir/flash.bin" bs=65536 count=768 2>/dev/null | sha | cut -d' ' -f1)
there=$(r 'dd if=/dev/mtd0 bs=65536 count=768 2>/dev/null | sha256sum' | cut -d' ' -f1)
[ "$here" = "$there" ] || stop "the copy differs from the flash: run it again"

part() { dd if="$dir/flash.bin" of="$dir/$1" bs=4096 skip=$(($2 / 4096)) count=$(($3 / 4096)) 2>/dev/null; }
part u-boot.bin     0x0       0xe0000
part u-boot-env.bin 0xe0000   0x20000
part kernel.bin     0x100000  0x580000
part rofs.bin       0x680000  0x2980000
part rwfs.bin       0x3000000 0x1000000
(cd "$dir" && sha u-boot.bin u-boot-env.bin kernel.bin rofs.bin rwfs.bin flash.bin > SHA256SUMS)
echo "dump: $dir"

got=$(sha "$dir/u-boot.bin" | cut -d' ' -f1)
[ "$got" = "$UBOOT_SHA" ] || stop "U-Boot differs from the tested one ($got): do NOT install, the dump is kept"
echo "U-Boot: the tested one. OK to go on."
