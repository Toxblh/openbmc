#!/bin/sh
# Rebuilds files/www/vendor/novnc/novnc.js from noVNC sources.
# The BMC serves every file over a separate TLS request and bmcweb drops
# connections whose TLS handshake takes longer than ~5 s, so ~40 separate ES
# modules of noVNC are bundled into one file (also smaller after gzip).
#
# Usage: tools/build-novnc-bundle.sh   (needs git, node/npx)
set -e
VERSION=v1.6.0
HERE=$(cd "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
git clone -q --depth=1 -b $VERSION https://github.com/novnc/noVNC "$TMP/noVNC"
cat > "$TMP/entry.js" <<EOT
export { default as RFB } from "$TMP/noVNC/core/rfb.js";
export { default as KeyTable } from "$TMP/noVNC/core/input/keysym.js";
export { default as Display } from "$TMP/noVNC/core/display.js";
EOT
OUT="$HERE/files/www/vendor/novnc"
mkdir -p "$OUT"
{
  echo "/* noVNC $VERSION (https://github.com/novnc/noVNC/tree/$VERSION), MPL-2.0, see LICENSE.txt."
  echo "   Bundled and minified by tools/build-novnc-bundle.sh; source code is available at the URL above. */"
  (cd "$TMP" && npx --yes esbuild@0.25.10 entry.js --bundle --format=esm --minify --target=es2022 --legal-comments=inline)
} > "$OUT/novnc.js"
cp "$TMP/noVNC/LICENSE.txt" "$OUT/LICENSE.txt"
echo "written $OUT/novnc.js"
