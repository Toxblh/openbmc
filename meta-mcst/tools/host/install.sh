#!/bin/sh
# install.sh: installs the REIMU host agent (reimu-hostd) on the Elbrus host
# and pairs it with the BMC. The BMC serves it (https://BMC/agent/install.sh),
# puts it on its virtual USB disk «REIMU-AGENT», and its web page Хост gives
# the command with the pairing string:
#
#   curl -fsS --pinnedpubkey 'sha256//…' -k https://BMC/agent/install.sh | sudo sh -s -- --pair 'reimu1:…'
#   sudo sh /run/media/$USER/REIMU-AGENT/install.sh
#   sudo sh install.sh [--pair 'reimu1:…'] [--url https://BMC]
#   sudo sh install.sh --uninstall [--purge]
#
# The agent's files come from (first that applies): the end of this file (the
# page's «Скачать установщик»), the directory of this file (the USB disk, a
# copy of meta-mcst/tools/host), the BMC over HTTPS (curl, wget or python3;
# its IPv6 link-local address on every interface that is up, then its IPv4,
# both from the pairing string; or --url). Each file is checked against its
# SHA-256 below, written in at the BMC's build: this file is fetched over the
# pinned key, the files it fetches need no trust of their own.
# Once paired, with a video card besides the ASPEED: the picture services of
# the web KVM (reimu-h264, reimu-fbmirror; reimu-hostd.py kvm install), set up,
# enabled and restarted by every install or update; KVM_FILES are optional
# (an older BMC's bundle or page has none: then they are left as they are).
# Installed: /usr/local/lib/reimu/reimu-hostd.py, the unit
# /etc/systemd/system/reimu-hostd.service, /etc/reimu-hostd.conf (kept when
# there), the key /etc/reimu-hostd/secret (by the pairing), the udev rule
# /etc/udev/rules.d/60-reimu-bmc-hub.rules (the BMC's USB hub never suspended);
# once paired /etc/modprobe.d/reimu-bmc-owns-i2c.conf and pwmd disabled (the BMC
# the only master of the shared I2C buses; --uninstall gives both back). Again: an update
# (with --pair: a new pairing). All of it is one { … } block: the shell reads
# the whole of it before running any of it, so a download cut short runs
# nothing, and the USB disk may go away while it runs.
#
# A host without systemd (no /run/systemd/system or no systemctl): instead of
# the unit /etc/init.d/reimu-hostd (reimu-hostd.init), started at boot by
# update-rc.d, chkconfig, insserv or rc?.d links, log /var/log/reimu-hostd.log.
#
# REIMU_ROOT=DIR (tests): every path under DIR, root not needed, systemctl
# only when REIMU_SYSTEMCTL names one; REIMU_SYS: /sys elsewhere. systemd is
# taken for granted unless REIMU_RUN_SYSTEMD names the directory to look for;
# update-rc.d, chkconfig, insserv only from the directory REIMU_RC_BIN;
# REIMU_LINK_WAIT: seconds to wait for the link.

{
REIMU_SUMS='
@SHA256SUMS@
'
# the self-contained installer: the pairing string written in by the page
REIMU_PAIR_DEFAULT=''

FILES="reimu-hostd.py reimu-hostd.service reimu-hostd.conf reimu-hostd.init"
# the picture services the agent sets up (optional: an older bundle lacks them)
KVM_FILES="reimu-h264.py reimu-host.sh reimu-fbmirror.c"

say() { printf '%s\n' "$*"; }
# ru, then en
msg() { printf '%s\n    %s\n' "$1" "$2"; }
die() { # code ru en
    code=$1; shift
    printf 'Ошибка: %s\nError: %s\n' "$1" "$2" >&2
    exit "$code"
}
have() { command -v "$1" >/dev/null 2>&1; }
# reimu1: and 16..2000 of base64url (no {m,n} in a regex: BSD grep stops at 255)
pair_ok() {
    case $1 in reimu1:*) ;; *) return 1 ;; esac
    case ${1#reimu1:} in *[!A-Za-z0-9_-]*) return 1 ;; esac
    [ ${#1} -ge 23 ] && [ ${#1} -le 2007 ]
}

sha256_of() {
    if have sha256sum; then sha256sum "$1" | cut -d' ' -f1
    elif have shasum; then shasum -a 256 "$1" | cut -d' ' -f1
    else python3 -c 'import hashlib,sys; print(hashlib.sha256(open(sys.argv[1],"rb").read()).hexdigest())' "$1"
    fi
}

optional() { case " $KVM_FILES " in *" $1 "*) return 0 ;; esac; return 1; }

sum_wanted() { # name -> its SHA-256 from REIMU_SUMS
    printf '%s\n' "$REIMU_SUMS" | while read -r h n; do
        [ "$n" = "$1" ] && { echo "$h"; break; }
    done
}

unbuilt() {
    case $REIMU_SUMS in *@SHA256''SUMS@*) return 0 ;; esac
    return 1
}

# ------------------------------------------------------------------ downloads
fetch_curl() { curl -fsS -k -g --connect-timeout 5 --max-time 120 -o "$2" "$1" 2>/dev/null; }
fetch_wget() { wget -q --no-check-certificate -T 10 -t 1 -O "$2" "$1" 2>/dev/null; }
fetch_python3() {
    python3 - "$1" "$2" <<'PY' 2>/dev/null
import http.client, socket, ssl, sys, urllib.parse
u = urllib.parse.urlsplit(sys.argv[1])
host = urllib.parse.unquote(u.netloc.rsplit("]", 1)[0].lstrip("[") if u.netloc.startswith("[") else u.hostname)
port = u.port or 443
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
ctx.check_hostname, ctx.verify_mode = False, ssl.CERT_NONE  # the SHA-256 of each file is checked
c = http.client.HTTPConnection(host.split("%")[0] if ":" in host else host, port, timeout=10)
c.sock = ctx.wrap_socket(socket.create_connection((host, port), timeout=10))
c.request("GET", u.path)
r = c.getresponse()
if r.status != 200:
    sys.exit(1)
open(sys.argv[2], "wb").write(r.read())
PY
}

downloaders() {
    for d in curl wget python3; do have $d && echo $d; done
}

# the BMC's addresses to try, best first (the agent's own order)
urls() {
    [ -n "$URL" ] && { echo "${URL%/}"; return 0; }
    [ -n "$PAIR" ] || return 0
    python3 - "$PAIR" "${REIMU_SYS:-/sys}" <<'PY'
import base64, json, os, socket, sys
b = sys.argv[1][7:]
d = json.loads(base64.urlsafe_b64decode(b + "=" * (-len(b) % 4)))
mac, v4 = d.get("mac") or "", d.get("v4") or ""
try:
    m = bytearray(int(x, 16) for x in mac.split(":"))
    assert len(m) == 6
    m[0] ^= 2
    ll = socket.inet_ntop(socket.AF_INET6, b"\xfe\x80" + b"\0" * 6 + bytes(m[:3]) + b"\xff\xfe" + bytes(m[3:]))
except (ValueError, AssertionError):
    ll = None
net = os.path.join(sys.argv[2], "class", "net")
rd = lambda p: open(p).read().strip() if os.path.exists(p) else ""
if ll and os.path.isdir(net):
    for n in sorted(os.listdir(net)):
        if n == "lo" or rd(os.path.join(net, n, "type")) == "772":
            continue
        if rd(os.path.join(net, n, "operstate")) in ("up", "unknown") and rd(os.path.join(net, n, "carrier")) != "0":
            print("https://[%s%%25%s]" % (ll, n))
if v4:
    print("https://%s" % v4)
PY
}

get_net() { # dir
    unbuilt && die 3 "этот install.sh не собран (нет SHA-256 файлов): возьмите его с BMC (https://BMC/agent/install.sh) или запустите из каталога с файлами агента" \
        "this install.sh is unbuilt (no SHA-256 of the files): take it from the BMC (https://BMC/agent/install.sh) or run it from a directory with the agent's files"
    list=$(urls)
    [ -n "$list" ] || die 2 "не знаю, где BMC: нужна строка привязки (--pair) или --url https://BMC" \
        "no BMC address: give the pairing string (--pair) or --url https://BMC"
    dls=$(downloaders)
    [ -n "$dls" ] || die 3 "нет ни curl, ni wget, ni python3" "neither curl, wget nor python3"
    for u in $list; do
        for d in $dls; do
            first=${FILES%% *}
            if fetch_$d "$u/agent/$first" "$1/$first"; then
                for f in $FILES; do
                    [ "$f" = "$first" ] && continue
                    fetch_$d "$u/agent/$f" "$1/$f" || die 3 "не удалось скачать $u/agent/$f" "could not download $u/agent/$f"
                done
                for f in $KVM_FILES; do
                    [ -n "$(sum_wanted "$f")" ] || continue
                    fetch_$d "$u/agent/$f" "$1/$f" || rm -f "$1/$f"
                done
                msg "Файлы агента скачаны с $u ($d)" "downloaded with $d from $u"
                return 0
            fi
        done
    done
    die 3 "BMC не отвечает ни по одному адресу: $(echo $list)" "the BMC is not reachable at any of: $(echo $list)"
}

get_payload() { # dir: the files after #__REIMU_PAYLOAD__ in this file
    for f in $FILES $KVM_FILES; do
        awk -v n="$f" 'p && $1 == n { print $2 } $0 == "#__REIMU_PAYLOAD__" { p = 1 }' "$SELF" > "$1/$f.b64"
        if [ ! -s "$1/$f.b64" ] && optional "$f"; then
            rm -f "$1/$f.b64"
            continue
        fi
        if have base64; then base64 -d < "$1/$f.b64" > "$1/$f" 2>/dev/null
        else python3 -c 'import base64,sys; sys.stdout.buffer.write(base64.b64decode(sys.stdin.read()))' < "$1/$f.b64" > "$1/$f"
        fi || die 3 "встроенный файл $f повреждён" "the embedded $f is damaged"
        rm -f "$1/$f.b64"
    done
    msg "Файлы агента взяты из самого установщика" "the agent's files from the installer itself"
}

get_local() { # dir
    for f in $FILES; do cp "$SRC/$f" "$1/$f" || die 3 "не удалось прочитать $SRC/$f" "could not read $SRC/$f"; done
    for f in $KVM_FILES; do [ ! -f "$SRC/$f" ] || cp "$SRC/$f" "$1/$f" || die 3 "не удалось прочитать $SRC/$f" "could not read $SRC/$f"; done
    msg "Файлы агента взяты из $SRC" "the agent's files from $SRC"
}

verify() { # dir
    unbuilt && return 0 # a copy of the source tree: the files next to it are as trusted as it is
    for f in $FILES $KVM_FILES; do
        [ -f "$1/$f" ] || ! optional "$f" || continue
        want=$(sum_wanted "$f")
        got=$(sha256_of "$1/$f")
        [ -n "$want" ] && [ "$want" = "$got" ] || die 3 "SHA-256 файла $f не совпадает с указанным в install.sh: файл подменён или повреждён, ничего не установлено" \
            "SHA-256 mismatch of $f (got ${got:-none}, want ${want:-none}): nothing installed"
    done
}

# ------------------------------------------------------------------ install
put() { # src dst mode: atomically
    mkdir -p "$(dirname "$2")" && cp "$1" "$2.new" && chmod "$3" "$2.new" && mv -f "$2.new" "$2" ||
        die 1 "не удалось записать $2" "could not write $2"
}

systemctl_() {
    if [ -n "$REIMU_ROOT" ]; then
        [ -n "${REIMU_SYSTEMCTL:-}" ] || return 0
        "$REIMU_SYSTEMCTL" "$@"
    else
        systemctl "$@"
    fi
}

journal() {
    if [ -n "$REIMU_ROOT" ]; then
        [ -n "${REIMU_JOURNALCTL:-}" ] && "$REIMU_JOURNALCTL" "$@"
    else
        journalctl "$@"
    fi
}

paired() { [ -f "$SECRET" ] && grep -q '^KEY=' "$SECRET"; }

# ------------------------------------------------------------------ no systemd
detect_init() {
    INIT=systemd
    if [ -n "$REIMU_ROOT" ]; then
        if [ -n "${REIMU_RUN_SYSTEMD:-}" ]; then
            [ -d "$REIMU_RUN_SYSTEMD" ] && [ -n "${REIMU_SYSTEMCTL:-}" ] || INIT=sysv
        fi
    else
        [ -d /run/systemd/system ] && have systemctl || INIT=sysv
    fi
    if [ "$INIT" = sysv ] && [ -z "$REIMU_ROOT" ]; then
        PATH=$PATH:/usr/local/sbin:/usr/sbin:/sbin # su without -: no sbin
        export PATH
    fi
    REIMU_INIT=$INIT # the agent's commands below choose as this does
    export REIMU_INIT
}

rc_tool() { # name -> its path
    if [ -n "$REIMU_ROOT" ]; then
        [ -n "${REIMU_RC_BIN:-}" ] && [ -x "$REIMU_RC_BIN/$1" ] && echo "$REIMU_RC_BIN/$1"
    else
        command -v "$1" 2>/dev/null
    fi
}

rc_base() { # the directory with rc3.d
    for b in "$REIMU_ROOT/etc/rc.d" "$REIMU_ROOT/etc"; do
        [ -d "$b/rc3.d" ] && { echo "$b"; return 0; }
    done
    return 1
}

boot_linked() { # base -> a start link in a multi-user runlevel?
    for l in "$1"/rc[2345].d/S*reimu-hostd; do
        [ -e "$l" ] || [ -L "$l" ] && return 0
    done
    return 1
}

boot_on() {
    how=
    if t=$(rc_tool update-rc.d) && "$t" reimu-hostd defaults >/dev/null 2>&1; then how=update-rc.d
    elif t=$(rc_tool chkconfig) && "$t" --add reimu-hostd >/dev/null 2>&1 && "$t" reimu-hostd on >/dev/null 2>&1; then how=chkconfig
    elif t=$(rc_tool insserv) && "$t" reimu-hostd >/dev/null 2>&1; then how=insserv
    fi
    # a tool's exit code is no proof: where the rc?.d are known, look for its link
    if b=$(rc_base) && { [ -z "$how" ] || ! boot_linked "$b"; }; then
        target=${INITD#"$REIMU_ROOT"}/reimu-hostd
        for l in 3 5; do mkdir -p "$b/rc$l.d" && ln -sf "$target" "$b/rc$l.d/S99reimu-hostd"; done
        for l in 0 6; do mkdir -p "$b/rc$l.d" && ln -sf "$target" "$b/rc$l.d/K01reimu-hostd"; done
        how="${b#"$REIMU_ROOT"}/rc?.d"
    fi
    if [ -n "$how" ]; then
        msg "Запуск при загрузке включён ($how)" "started at boot ($how)"
    else
        msg "ВНИМАНИЕ: нет ни update-rc.d, ни chkconfig, ни insserv, ни каталога rc3.d: агент НЕ будет запускаться при загрузке; запускайте его сами: ${INITD#"$REIMU_ROOT"}/reimu-hostd start" \
            "WARNING: no update-rc.d, chkconfig, insserv or rc3.d directory: the agent will NOT start at boot; start it yourself: ${INITD#"$REIMU_ROOT"}/reimu-hostd start"
    fi
}

boot_off() {
    if t=$(rc_tool update-rc.d); then "$t" -f reimu-hostd remove >/dev/null 2>&1; fi
    if t=$(rc_tool chkconfig); then "$t" --del reimu-hostd >/dev/null 2>&1; fi
    if t=$(rc_tool insserv); then "$t" -r reimu-hostd >/dev/null 2>&1; fi
    for b in "$REIMU_ROOT/etc/rc.d" "$REIMU_ROOT/etc"; do
        rm -f "$b"/rc?.d/[SK][0-9][0-9]reimu-hostd
    done
}

log_size() { # the log's size now
    [ -f "$LOGF" ] || { echo 0; return 0; }
    set -- $(wc -c < "$LOGF")
    echo "${1:-0}"
}

start_sysv() {
    show=${INITD#"$REIMU_ROOT"}/reimu-hostd
    logshow=${LOGF#"$REIMU_ROOT"}
    boot_on
    since=$(log_size)
    sh "$INITD/reimu-hostd" restart >/dev/null || die 1 "служба reimu-hostd не запустилась: $logshow, $show status" \
        "reimu-hostd did not start: $logshow, $show status"
    msg "Служба reimu-hostd запущена, жду связи с BMC…" "reimu-hostd started, waiting for the BMC…"
    i=0
    while [ $i -lt "${REIMU_LINK_WAIT:-20}" ]; do
        [ "$(log_size)" -ge "$since" ] || since=0 # its loop set the old log aside
        line=$(tail -c +$((since + 1)) "$LOGF" 2>/dev/null | grep -E 'connected to the BMC|refused|not reachable' | tail -n 1)
        [ -n "$line" ] && break
        sleep 1
        i=$((i + 1))
    done
    case $line in
        *"connected to the BMC"*)
            msg "Готово: $line. На странице BMC «Хост» агент «подключён»." "done: $line" ;;
        *)
            msg "Служба работает, но связи с BMC пока нет: tail -f $logshow" \
                "the service runs, no link to the BMC yet: tail -f $logshow${line:+ ($line)}" ;;
    esac
    sh "$INITD/reimu-hostd" status 2>/dev/null | head -n 3
    return 0
}

do_install() {
    have python3 || die 1 "нужен python3 (агент написан на нём)" "python3 is needed (the agent is written in it)"
    python3 -c 'import sys; sys.exit(sys.version_info < (3, 6))' || die 1 "нужен python3 версии 3.6 или новее" "python3 3.6 or newer is needed"
    T=$(mktemp -d "${TMPDIR:-/tmp}/reimu-agent.XXXXXX") || die 1 "mktemp" "mktemp"
    trap 'rm -rf "$T"' EXIT
    if [ -n "$SELF" ] && grep -q '^#__REIMU_PAYLOAD__$' "$SELF" 2>/dev/null; then get_payload "$T"
    elif [ -n "$SRC" ]; then get_local "$T"
    else get_net "$T"
    fi
    verify "$T"
    put "$T/reimu-hostd.py" "$LIB/reimu-hostd.py" 755
    if [ "$INIT" = systemd ]; then
        put "$T/reimu-hostd.service" "$UNIT" 644
    else
        put "$T/reimu-hostd.init" "$INITD/reimu-hostd" 755
    fi
    if [ -f "$CONF" ]; then
        msg "Настройки $CONF сохранены" "$CONF kept"
    else
        put "$T/reimu-hostd.conf" "$CONF" 644
    fi
    msg "Агент установлен в $LIB" "the agent installed in $LIB"
    # the BMC's virtual USB hub (KVM keyboard/mouse) never autosuspended: the
    # udev rule (the agent's) and the hub there now; the agent keeps it so too
    if REIMU_HOSTD_ROOT="$REIMU_ROOT" python3 "$LIB/reimu-hostd.py" hub-rule install; then
        msg "USB-хаб BMC (клавиатура и мышь KVM) больше не засыпает" "the BMC's USB hub (KVM keyboard/mouse) kept awake (udev rule)"
    else
        msg "Не удалось поставить правило udev для USB-хаба BMC (агент всё равно держит его включённым)" \
            "the udev rule for the BMC's USB hub not installed (the agent keeps the hub awake anyway)"
    fi
    if [ -n "$PAIR" ]; then
        msg "Привязка к BMC…" "pairing with the BMC…"
        if ! python3 "$LIB/reimu-hostd.py" pair --pair "$PAIR" --config "$CONF" --secret "$SECRET"; then
            printf '%s\n' "Ошибка: BMC не принял привязку (ключ устарел — он действует 15 минут и один раз — или BMC недоступен)." \
                "  Получите новую строку на странице BMC «Хост» (кнопка «Новый ключ») и запустите команду снова." \
                "Error: pairing refused or the BMC not reachable: get a new pairing string on the BMC's page Хост and run again." >&2
            exit 4
        fi
        msg "Привязан" "paired"
    fi
    sync
    if ! paired; then
        msg "Агент не привязан: запустите снова с --pair 'reimu1:…' (страница BMC «Хост» → «Подключить агент хоста»)" \
            "not paired: run again with --pair 'reimu1:…' (the BMC's page Хост)"
        return 0
    fi
    # the BMC the only master of the I2C buses shared with the host (the agent's
    # text, pwmd's state written down there); after the pairing: until then
    # no agent watches and the host's own fan control stays
    if REIMU_HOSTD_ROOT="$REIMU_ROOT" REIMU_HOSTD_CONF="$CONF" python3 "$LIB/reimu-hostd.py" bus-owner install; then
        msg "Шины I²C платформы теперь только у BMC: при выключенном BMC или старой прошивке вентиляторами никто не управляет" \
            "the shared I2C buses are the BMC's alone: with the BMC down or on old firmware nobody drives the fans"
    else
        msg "Не удалось передать шины I²C платформы BMC (агент всё равно отключает драйверы хоста)" \
            "the modprobe.d file or pwmd not handled (the agent unbinds the host's drivers anyway)"
    fi
    # the picture services of the web KVM (H.264 from the video card, the mirror
    # into the ASPEED VGA): set up, enabled and restarted with every install or
    # update; only with a video card besides the ASPEED (the agent decides,
    # its lines say what it did)
    REIMU_HOSTD_ROOT="$REIMU_ROOT" REIMU_HOSTD_CONF="$CONF" python3 "$LIB/reimu-hostd.py" kvm install --from "$T" ||
        msg "Службы картинки KVM (H.264 с видеокарты) не настроены: журнал выше" \
            "the KVM picture services (H.264 from the video card) not set up: see above"
    if [ "$INIT" = sysv ]; then
        start_sysv
        return 0
    fi
    since=$(date '+%Y-%m-%d %H:%M:%S')
    systemctl_ daemon-reload
    systemctl_ enable reimu-hostd.service >/dev/null 2>&1 || systemctl_ enable reimu-hostd.service
    systemctl_ restart reimu-hostd.service || die 1 "служба reimu-hostd не запустилась: journalctl -u reimu-hostd" \
        "reimu-hostd.service did not start: journalctl -u reimu-hostd"
    if [ -n "$REIMU_ROOT" ] && [ -z "${REIMU_SYSTEMCTL:-}" ]; then return 0; fi
    msg "Служба reimu-hostd запущена, жду связи с BMC…" "reimu-hostd.service started, waiting for the BMC…"
    i=0
    while [ $i -lt 20 ]; do
        line=$(journal -u reimu-hostd.service --since "$since" -n 50 -o cat --no-pager 2>/dev/null | grep -E 'connected to the BMC|refused|not reachable' | tail -n 1)
        [ -n "$line" ] && break
        [ -n "$REIMU_ROOT" ] || sleep 1
        i=$((i + 1))
    done
    case $line in
        *"connected to the BMC"*)
            msg "Готово: $line. На странице BMC «Хост» агент «подключён»." "done: $line" ;;
        *)
            msg "Служба работает, но связи с BMC пока нет: journalctl -u reimu-hostd -f" \
                "the service runs, no link to the BMC yet: journalctl -u reimu-hostd -f${line:+ ($line)}" ;;
    esac
    systemctl_ --no-pager --lines=0 status reimu-hostd.service 2>/dev/null | head -n 3
    return 0
}

do_uninstall() {
    [ "$INIT" = sysv ] || systemctl_ disable --now reimu-hostd.service 2>/dev/null
    if [ -f "$INITD/reimu-hostd" ]; then
        sh "$INITD/reimu-hostd" stop >/dev/null 2>&1
        boot_off
    fi
    # the agent's picture services (only those it set up: reimu-host.sh's own stay)
    [ ! -f "$LIB/reimu-hostd.py" ] ||
        REIMU_HOSTD_ROOT="$REIMU_ROOT" python3 "$LIB/reimu-hostd.py" kvm remove 2>/dev/null
    # the hub rule stays while reimu-host.sh (the KVM side) is installed
    [ -f "$LIB/reimu-host.sh" ] || [ ! -f "$LIB/reimu-hostd.py" ] ||
        REIMU_HOSTD_ROOT="$REIMU_ROOT" python3 "$LIB/reimu-hostd.py" hub-rule remove 2>/dev/null
    # the agent is stopped by now, or it would stop pwmd again; nothing is unbound
    if [ -f "$LIB/reimu-hostd.py" ]; then
        REIMU_HOSTD_ROOT="$REIMU_ROOT" REIMU_HOSTD_CONF="$CONF" python3 "$LIB/reimu-hostd.py" bus-owner remove 2>/dev/null
        msg "Управление вентиляторами возвращено хосту: драйверы вернутся после перезагрузки хоста" \
            "fan control given back to the host: its drivers come back with the host's next reboot"
    fi
    rm -f "$UNIT" "$LIB/reimu-hostd.py" "$SECRET" "$INITD/reimu-hostd" "$LOGF" "$LOGF.1" \
        "$REIMU_ROOT/var/run/reimu-hostd.pid" "$REIMU_ROOT/var/run/reimu-hostd.agent.pid"
    rm -rf "$REIMU_ROOT/var/run/reimu-hostd.lock"
    rm -f "$REIMU_ROOT/var/lib/reimu-hostd/policy" "$REIMU_ROOT/var/lib/reimu-hostd/policy.new"
    rmdir "$(dirname "$SECRET")" "$LIB" "$REIMU_ROOT/var/lib/reimu-hostd" 2>/dev/null
    [ "$PURGE" = 1 ] && rm -f "$CONF"
    [ "$INIT" = sysv ] || systemctl_ daemon-reload 2>/dev/null
    msg "Агент удалён$([ "$PURGE" = 1 ] || echo " (настройки $CONF оставлены)"). Отозвать ключ на BMC: страница «Хост» → «Отключить агент»." \
        "removed$([ "$PURGE" = 1 ] || echo " ($CONF kept)"); revoke the key on the BMC: page Хост → «Отключить агент»"
}

usage() {
    sed -n '2,13p' "${SELF:-/dev/null}" 2>/dev/null | sed 's/^# \{0,1\}//'
    [ -n "$SELF" ] || say "usage: sh install.sh [--pair 'reimu1:…'] [--url https://BMC] | --uninstall [--purge]"
}

main() {
    REIMU_ROOT=${REIMU_ROOT:-}
    LIB=$REIMU_ROOT/usr/local/lib/reimu
    UNIT=$REIMU_ROOT/etc/systemd/system/reimu-hostd.service
    CONF=$REIMU_ROOT/etc/reimu-hostd.conf
    SECRET=$REIMU_ROOT/etc/reimu-hostd/secret
    INITD=$REIMU_ROOT/etc/init.d
    [ -d "$INITD" ] || [ ! -d "$REIMU_ROOT/etc/rc.d/init.d" ] || INITD=$REIMU_ROOT/etc/rc.d/init.d
    LOGF=$REIMU_ROOT/var/log/reimu-hostd.log
    PAIR=$REIMU_PAIR_DEFAULT URL=${REIMU_URL:-} ACTION=install PURGE=
    while [ $# -gt 0 ]; do
        case $1 in
            --pair) PAIR=${2:-}; shift ;;
            --pair=*) PAIR=${1#*=} ;;
            --url) URL=${2:-}; shift ;;
            --url=*) URL=${1#*=} ;;
            --uninstall) ACTION=uninstall ;;
            --purge) PURGE=1 ;;
            --print-urls) ACTION=urls ;;
            -h|--help) ACTION=help ;;
            *) die 2 "неизвестный параметр $1" "unknown option $1" ;;
        esac
        shift
    done
    # run from a file (not piped): its directory may have the files (the USB disk)
    SELF= SRC=
    [ -f "$0" ] && grep -q '^REIMU_PAIR_DEFAULT=' "$0" 2>/dev/null && SELF=$0
    if [ -n "$SELF" ] && [ -z "$URL" ]; then
        d=$(cd "$(dirname "$SELF")" && pwd)
        ok=1
        for f in $FILES; do [ -f "$d/$f" ] || ok=; done
        [ -n "$ok" ] && SRC=$d
        if [ -z "$PAIR" ] && [ -f "$d/PAIRING.TXT" ]; then
            PAIR=$(head -n 1 "$d/PAIRING.TXT" | tr -d ' \r\t')
            case $PAIR in REIMU-PAIRING-*) PAIR= ;; esac
        fi
    fi
    if [ -n "$PAIR" ]; then
        pair_ok "$PAIR" || die 2 "это не строка привязки (reimu1:…): скопируйте её со страницы BMC «Хост»" \
            "not a pairing string (reimu1:…): copy it from the BMC's web page Хост"
    fi
    case $ACTION in
        help) usage; return 0 ;;
        urls) urls; return 0 ;;
    esac
    if [ -z "$REIMU_ROOT" ] && [ "$(id -u)" != 0 ]; then
        die 1 "нужны права root: sudo sh install.sh …" "run as root: sudo sh install.sh …"
    fi
    detect_init
    case $ACTION in
        install) do_install ;;
        uninstall) do_uninstall ;;
    esac
}

main "$@"; exit $?
}
