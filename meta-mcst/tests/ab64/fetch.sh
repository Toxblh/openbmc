#!/bin/sh
# fetch.sh [WORKDIR]: the latest A/B build out of the build container "reimu"
# into WORKDIR (default: here): ab.mtd (the build's .ab.mtd with the U-Boot
# dumped from the real BMC, $STOCK/u-boot.bin, as on the board),
# reimu-ab.tar (the update), reimu-ab-kexec.tar (to start it from RAM)
set -e
cd "${1:-.}"
D=/work/build-ab/tmp/deploy/images/reimu-5564
docker cp reimu:$D/$(docker exec reimu readlink $D/obmc-phosphor-image-reimu-5564.ab.mtd) ab-build.mtd
docker cp -L reimu:$D/reimu-ab.tar reimu-ab.tar
docker cp -L reimu:$D/reimu-ab-kexec.tar reimu-ab-kexec.tar
python3 - ab-build.mtd "${STOCK:-$HOME/git/new-reimu/backup}/u-boot.bin" ab.mtd <<'PY'
import sys
img = bytearray(open(sys.argv[1], 'rb').read()); ub = open(sys.argv[2], 'rb').read()
img[0:len(ub)] = ub; open(sys.argv[3], 'wb').write(img)
PY
tar -xOf reimu-ab.tar SIZES
