#!/bin/sh
# make-bundle.sh DEPLOY_DIR OUT: the package to install on another board, from
# a finished build: reimu-ab-<version>.tar.gz, its .sha256, RELEASE.md.
set -eu
deploy=$1; out=$2
here=$(cd "$(dirname "$0")" && pwd); tests=$here/../../tests/ab64
ver=$(cd "$here" && git describe --dirty)
n=reimu-ab-$ver; d=$out/$n
python3 "$here/check-image.py" "$deploy/reimu-ab.tar"
mkdir -p "$d"
cp "$deploy/reimu-ab.tar" "$deploy/reimu-ab-kexec.tar" "$d/"
cp "$here/reimu-ab-from-stock" "$here/backup-stock.sh" "$tests/board-check.sh" "$d/"
sed "s/@VERSION@/$ver/g" "$here/INSTALL.ru.md" > "$d/README.md"
chmod +x "$d/reimu-ab-from-stock" "$d/backup-stock.sh"
(cd "$d" && sha256sum reimu-ab.tar reimu-ab-kexec.tar reimu-ab-from-stock backup-stock.sh board-check.sh > SHA256SUMS)
tar -C "$out" -czf "$out/$n.tar.gz" "$n"
(cd "$out" && sha256sum "$n.tar.gz" > "$n.tar.gz.sha256")
sed -n '1,/^## Прочитайте/p' "$d/README.md" | sed '$d' > "$out/RELEASE.md"
printf 'Инструкция: `README.md` внутри архива.\n\n```\n%s\n```\n' "$(cat "$d/SHA256SUMS")" >> "$out/RELEASE.md"
rm -rf "$d"
ls -la "$out"
