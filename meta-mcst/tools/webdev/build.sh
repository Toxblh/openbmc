#!/bin/bash
# Builds the REIMU web devkit: the new web UI plus the BMC-side programs it
# needs (patched bmcweb, nbd-proxy, reimu-serial, reimu-pnsd), cross-compiled for the
# AST2500 (ARM1176JZ-S, soft float, glibc 2.32 as in REIMU's Yocto 3.2),
# packed to run from tmpfs on a BMC with the stock REIMU firmware. Nothing
# on the BMC flash is changed: see reimu-webdev (the control script).
#
# Usage: build.sh [work dir]      (default: ./webdev-work)
#   -> <work>/reimu-webdev-<version>.tar.gz
#
# TARGET=x86_64 STUB_LIBS=<dir with libsystemd.so.0, libudev.so.1,
# libpam.so.0> builds the same package for the x86 test VM
# (meta-mcst/tests/testvm), to test the devkit there.
#
# Needs: git, curl, python3 + pip, ninja, readelf/strip for ARM
# (binutils-arm-linux-gnueabi), dpkg-deb. No Yocto: the compiler is zig
# (from PyPI), which links against any glibc version; libsystemd, libudev
# and libpam are linked as stubs with the real symbol versions (the BMC's
# libraries are used at run time); openssl, zlib, tinyxml2 and json-c are
# linked statically.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
MCST=$(cd "$HERE/../.." && pwd)
OPENBMC=$(cd "$MCST/.." && pwd)
W=$(mkdir -p "${1:-webdev-work}" && cd "${1:-webdev-work}" && pwd)
SRC=$W/src SYS=$W/sysroot BIN=$W/bin OUT=$W/out
mkdir -p "$SRC" "$SYS/usr/lib/pkgconfig" "$SYS/usr/include" "$BIN"

srcrev() { grep -h '^SRCREV' "$1" | head -1 | cut -d'"' -f2; }
BMCWEB_REV=$(srcrev "$OPENBMC/meta-phosphor/recipes-phosphor/interfaces/bmcweb_git.bb")
SDBUSPLUS_REV=$(srcrev "$OPENBMC/meta-phosphor/recipes-extended/sdbusplus/sdbusplus-rev.inc")
JSNBD_REV=$(srcrev "$OPENBMC/meta-phosphor/recipes-connectivity/jsnbd/jsnbd_git.bb")
ZIG_VERSION=0.13.0
MESON_VERSION=0.57.2
BOOST_SHA=953db31e016db7bb207f11432bef7df100516eeb746843fa0486a222e3fd49cb  # boost_1_75_0.tar.bz2
UBUNTU=http://ports.ubuntu.com/ubuntu-ports/pool/main
DEBS="
s/systemd/libsystemd-dev_245.4-4ubuntu3_armhf.deb eed4369faa161ebf6446f63572f583a818302035fa5ec887d685661ae9999718
s/systemd/libsystemd0_245.4-4ubuntu3_armhf.deb d7150b4dadbf0612c35bc5e0b056a3aeec4f5a67225205d02b7206bb454c8562
p/pam/libpam0g-dev_1.3.1-5ubuntu4_armhf.deb 117ebb881ee1030e3fb4cd9e80b4e49a45fe74502097e1b897cde2e56cd8d961
p/pam/libpam0g_1.3.1-5ubuntu4_armhf.deb 6d6248683668f770cd79764ae75630285af7b52540d6a62bb103edab2b35dd84
s/systemd/libudev-dev_245.4-4ubuntu3_armhf.deb fa6e1ca02604f21b39670d3e2eee80df37b70dc121a07e7ee71ef259624a91b8
s/systemd/libudev1_245.4-4ubuntu3_armhf.deb 557a2bbf182c52712e7f36a48998258bb440528329b270d53c86a4e2a5ee27dd
"
TARGET=${TARGET:-arm}
case "$TARGET" in
    arm)    ZTARGET=arm-linux-gnueabi.2.32; ZFLAGS="-mcpu=arm1176jz_s -mfloat-abi=soft"  # = Yocto tune arm1176jzs (no VFP)
            SSLTARGET=linux-armv4; MCPU=arm; STRIP=arm-linux-gnueabi-strip ;;
    x86_64) ZTARGET=x86_64-linux-gnu.2.31; ZFLAGS=""; SSLTARGET=linux-x86_64; MCPU=x86_64; STRIP=strip ;;
    *) echo "TARGET: arm or x86_64" >&2; exit 1 ;;
esac
STUB_LIBS=${STUB_LIBS:-}

step() { echo "== $*"; }
gitat() # url dir rev
{
    [ -d "$2/.git" ] || git clone -q "$1" "$2"
    git -C "$2" checkout -q -f "$3" 2>/dev/null || { git -C "$2" fetch -q origin "$3" && git -C "$2" checkout -q -f "$3"; }
    git -C "$2" clean -qfdx
}

# ---------------------------------------------------------------- tools
step "tools: zig $ZIG_VERSION, meson $MESON_VERSION"
if [ ! -x "$W/tools/zig/ziglang/zig" ]; then
    python3 -m pip download -q --no-deps -d "$W/tools/dl" "ziglang==$ZIG_VERSION"
    python3 -m zipfile -e "$W"/tools/dl/ziglang-*.whl "$W/tools/zig"
    chmod +x "$W/tools/zig/ziglang/zig"
fi
ZIG=$W/tools/zig/ziglang/zig
if [ ! -x "$W/tools/venv/bin/meson" ]; then
    python3 -m venv "$W/tools/venv"
    "$W/tools/venv/bin/pip" install -q "meson==$MESON_VERSION"
fi
MESON=$W/tools/venv/bin/meson
for t in cc c++; do
    extra=""
    [ $t = c++ ] && extra="-Wno-enum-constexpr-conversion -D_LIBCPP_ENABLE_CXX17_REMOVED_UNARY_BINARY_FUNCTION"
    cat > "$BIN/arm-zig-$t" <<EOF
#!/bin/bash
# zig as the cross compiler for the BMC ($ZTARGET $ZFLAGS)
cpu="$ZFLAGS"
args=()
for a in "\$@"; do
    case "\$a" in
        -Wl,--version) echo "LLD 18.1.0 (compatible with GNU linkers)"; exit 0 ;;  # meson's linker probe (zig ld is lld)
        -dM) cpu= ; args+=("\$a") ;;                                              # meson's define probe
        -Wl,-rpath-link,*) ;;                                                     # not supported by zig
        *) args+=("\$a") ;;
    esac
done
# code of 2021 written for gcc: clang's newer warnings must not fail the build
exec "$ZIG" $t -target $ZTARGET \$cpu "\${args[@]}" -Wno-error $extra
EOF
    chmod +x "$BIN/arm-zig-$t"
done
printf '#!/bin/sh\nexec "%s" ar "$@"\n' "$ZIG" > "$BIN/arm-zig-ar"
printf '#!/bin/sh\nexec "%s" ranlib "$@"\n' "$ZIG" > "$BIN/arm-zig-ranlib"
chmod +x "$BIN/arm-zig-ar" "$BIN/arm-zig-ranlib"
export PATH="$BIN:$PATH"
CC=arm-zig-cc

# ---------------------------------------------------------------- sysroot
step "sysroot: headers and link stubs of libsystemd, libudev and libpam (Ubuntu focal)"
mkdir -p "$W/debs"
echo "$DEBS" | while read -r f sha; do
    [ -n "$f" ] || continue
    d="$W/debs/${f##*/}"
    [ -f "$d" ] || curl -sfL -o "$d" "$UBUNTU/$f"
    echo "$sha  $d" | sha256sum -c --quiet -
    dpkg-deb -x "$d" "$W/debs/root"
done
cp -r "$W/debs/root/usr/include/." "$SYS/usr/include/"
for l in libsystemd.so.0 libudev.so.1 libpam.so.0; do
    python3 "$HERE/mkstub.py" "${STUB_LIBS:-$W/debs/root/lib/arm-linux-gnueabihf}/$l" "$SYS/usr/lib" $CC
done
echo | $CC -x c -c - -o "$W/empty.o" && arm-zig-ar rcs "$SYS/usr/lib/libatomic.a" "$W/empty.o"  # atomics: compiler-rt
cat > "$SYS/usr/lib/pkgconfig/libsystemd.pc" <<EOF
Name: libsystemd
Description: link stub of the BMC's libsystemd
Version: 246
Libs: -L$SYS/usr/lib -lsystemd
Cflags: -isystem $SYS/usr/include
EOF
cat > "$SYS/usr/lib/pkgconfig/systemd.pc" <<EOF
Name: systemd
Description: systemd paths on the BMC
Version: 246
systemdsystemunitdir=/lib/systemd/system
EOF

step "static zlib, tinyxml2, openssl"
gitat https://github.com/madler/zlib.git "$SRC/zlib" v1.2.13
(cd "$SRC/zlib" && CC=$CC AR=arm-zig-ar RANLIB=arm-zig-ranlib CFLAGS=-O2 ./configure --static --prefix="$SYS/usr" >/dev/null &&
    make -s -j"$(nproc)" libz.a >/dev/null && make -s install >/dev/null)
gitat https://github.com/leethomason/tinyxml2.git "$SRC/tinyxml2" 8.0.0
(cd "$SRC/tinyxml2" && arm-zig-c++ -O2 -c tinyxml2.cpp -o tinyxml2.o && arm-zig-ar rcs "$SYS/usr/lib/libtinyxml2.a" tinyxml2.o &&
    cp tinyxml2.h "$SYS/usr/include/")
cat > "$SYS/usr/lib/pkgconfig/tinyxml2.pc" <<EOF
Name: tinyxml2
Description: tinyxml2 (static)
Version: 8.0.0
Libs: -L$SYS/usr/lib -ltinyxml2
Cflags: -isystem $SYS/usr/include
EOF
gitat https://github.com/openssl/openssl.git "$SRC/openssl" OpenSSL_1_1_1w
(cd "$SRC/openssl" && ./Configure $SSLTARGET no-asm no-shared no-tests no-engine no-dso no-async -O2 \
    --prefix="$SYS/usr" --openssldir=/etc/ssl CC=$CC AR=arm-zig-ar RANLIB=arm-zig-ranlib >/dev/null &&
    make -s -j"$(nproc)" build_libs >/dev/null 2>&1 && make -s install_dev >/dev/null 2>&1)
gitat https://github.com/json-c/json-c.git "$SRC/json-c" json-c-0.15-20200726
(mkdir -p "$SRC/json-c/build" && cd "$SRC/json-c/build" &&
    cmake .. -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=$MCPU -DCMAKE_C_COMPILER="$BIN/arm-zig-cc" \
        -DCMAKE_AR="$BIN/arm-zig-ar" -DCMAKE_RANLIB="$BIN/arm-zig-ranlib" -DCMAKE_BUILD_TYPE=MinSizeRel \
        -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF -DDISABLE_EXTRA_LIBS=ON -DCMAKE_INSTALL_PREFIX="$SYS/usr" >/dev/null &&
    make -s -j"$(nproc)" >/dev/null && make -s install >/dev/null)
sed -i 's|^Cflags: -I|Cflags: -isystem |' "$SYS"/usr/lib/pkgconfig/*.pc

cat > "$W/arm-zig.cross" <<EOF
[binaries]
c = '$BIN/arm-zig-cc'
cpp = '$BIN/arm-zig-c++'
ar = '$BIN/arm-zig-ar'
strip = '$STRIP'
pkgconfig = 'pkg-config'

[properties]
pkg_config_libdir = '$SYS/usr/lib/pkgconfig'

[built-in options]
c_args = ['-isystem', '$SYS/usr/include']
cpp_args = ['-isystem', '$SYS/usr/include']
c_link_args = ['-L$SYS/usr/lib']
cpp_link_args = ['-L$SYS/usr/lib']

[host_machine]
system = 'linux'
cpu_family = '$MCPU'
cpu = '$MCPU'
endian = 'little'
EOF

# ---------------------------------------------------------------- bmcweb
step "bmcweb $BMCWEB_REV + REIMU patches"
gitat https://github.com/openbmc/bmcweb.git "$SRC/bmcweb" "$BMCWEB_REV"
for p in "$MCST"/meta-common/recipes-phosphor/bmcweb/*.patch "$HERE"/patches/bmcweb-*.patch; do
    patch -s -d "$SRC/bmcweb" -p1 < "$p"
done
S=$SRC/bmcweb/subprojects
gitat https://github.com/openbmc/sdbusplus.git "$S/sdbusplus" "$SDBUSPLUS_REV"
gitat https://github.com/CPPAlliance/url.git "$S/boost-url" "$(sed -n 's/^revision = //p' "$S/boost-url.wrap")"
gitat https://github.com/nlohmann/json.git "$S/nlohmann" "$(sed -n 's/^revision = //p' "$S/nlohmann.wrap")"
if [ ! -d "$S/boost_1_75_0" ]; then
    [ -f "$W/boost.tar.bz2" ] || curl -sfL -o "$W/boost.tar.bz2" "${BOOST_URL:-https://tarballs.nixos.org/sha256/$BOOST_SHA}"
    echo "$BOOST_SHA  $W/boost.tar.bz2" | sha256sum -c --quiet -
    tar xjf "$W/boost.tar.bz2" -C "$S"
fi
(cd "$SRC/bmcweb" &&
    "$MESON" setup build --cross-file "$W/arm-zig.cross" --buildtype=minsize -Db_lto=false -Dwerror=false \
        -Ddefault_library=static -Dtests=disabled -Dyocto-deps=disabled \
        -Dreimu-serial-socket=enabled -Dvm-websocket=enabled -Dhttp-body-limit=64 >/dev/null &&
    ninja -C build bmcweb >/dev/null)

# ---------------------------------------------------------------- daemons
step "nbd-proxy $JSNBD_REV + REIMU patch, reimu-serial, reimu-pnsd"
gitat https://github.com/openbmc/jsnbd.git "$SRC/jsnbd" "$JSNBD_REV"
for p in "$MCST"/meta-common/recipes-connectivity/jsnbd/files/*.patch; do patch -s -d "$SRC/jsnbd" -p1 < "$p"; done
mkdir -p "$W/obj"
# configuration and state hook: /run/reimu-webdev/etc (set up by reimu-webdev)
printf '#define HAVE_SPLICE 1\n#define PACKAGE "nbd-proxy"\n#define SYSCONFDIR "/run/reimu-webdev/etc"\n#define RUNSTATEDIR "/run"\n' > "$W/obj/config.h"
$CC -O2 -Wall -I"$W/obj" -isystem "$SYS/usr/include" -isystem "$SYS/usr/include/json-c" \
    -o "$W/obj/nbd-proxy" "$SRC/jsnbd/nbd-proxy.c" -L"$SYS/usr/lib" -ljson-c -ludev
$CC -O2 -Wall -o "$W/obj/reimu-serial" "$MCST/meta-common/recipes-mcst/reimu-serial/files/reimu-serial.c"
$CC -O2 -Wall -isystem "$SYS/usr/include" -o "$W/obj/reimu-pnsd" "$MCST/meta-common/recipes-mcst/reimu-pns/files/reimu-pnsd.c" \
    -L"$SYS/usr/lib" -lsystemd

# ---------------------------------------------------------------- package
VERSION=$(git -C "$MCST" describe --always --dirty 2>/dev/null || echo unknown)
step "package $VERSION"
P=$OUT/reimu-webdev
rm -rf "$OUT" && mkdir -p "$P/bin" "$P/libexec" "$P/etc/dbus" "$P/etc/nbd-proxy"
$STRIP -o "$P/bin/bmcweb" "$SRC/bmcweb/build/bmcweb"
$STRIP -o "$P/bin/nbd-proxy" "$W/obj/nbd-proxy"
$STRIP -o "$P/bin/reimu-serial" "$W/obj/reimu-serial"
$STRIP -o "$P/bin/reimu-pnsd" "$W/obj/reimu-pnsd"
install -m755 "$MCST/meta-common/recipes-mcst/reimu-pns/files/reimu-pns-scan" "$P/libexec/"
install -m644 "$MCST/meta-common/recipes-mcst/reimu-pns/files/xyz.openbmc_project.Reimu.Pns.conf" "$P/etc/dbus/"
install -m644 "$MCST/meta-common/recipes-mcst/reimu-serial/files/reimu-serial.conf" "$P/etc/"
install -m644 "$MCST/meta-common/recipes-connectivity/jsnbd/files/config.json" "$P/etc/nbd-proxy/"
install -m755 "$MCST/meta-common/recipes-connectivity/jsnbd/files/reimu_state_hook" "$P/etc/nbd-proxy/state"
cp -r "$MCST/meta-common/recipes-phosphor/reimu-webui/files/www" "$P/www"
find "$P/www" -name 'LICENSE*' -delete
find "$P/www" -type f \( -name '*.js' -o -name '*.css' -o -name '*.html' -o -name '*.svg' \) -exec gzip -9 -n {} \;
install -m755 "$HERE/reimu-webdev" "$P/"
echo "$VERSION" > "$P/VERSION"

# the binaries must not need more than the BMC has
for b in "$P"/bin/*; do
    v=$(readelf -W -V "$b" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)
    printf '   %-14s %6s KiB  needs %s, %s\n' "${b##*/}" $(($(stat -c %s "$b") / 1024)) "${v:-?}" \
        "$(readelf -d "$b" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' | tr '\n' ' ')"
    case "$v" in GLIBC_2.3[3-9]*|GLIBC_2.[4-9]*) echo "   ERROR: $b needs $v, REIMU has glibc 2.32"; exit 1 ;; esac
    if [ "$TARGET" = arm ] && readelf -A "$b" | grep -q 'Tag_FP_arch'; then
        echo "   ERROR: $b uses VFP, the REIMU tune is arm1176jzs"; exit 1
    fi
done
PKG=$W/reimu-webdev-$VERSION$([ "$TARGET" = arm ] || echo "-$TARGET").tar.gz
tar czf "$PKG" -C "$OUT" reimu-webdev
echo "== $PKG: $(($(stat -c %s "$PKG") / 1024)) KiB, unpacked $(du -sk "$P" | cut -f1) KiB"
