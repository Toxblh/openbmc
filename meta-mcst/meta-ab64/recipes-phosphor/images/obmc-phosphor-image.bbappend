# Slimmed so that two copies (A/B) fit the 64 MiB flash: what goes and what it
# saved is in meta-mcst/tools/image-size (plan-reimu-5564.txt, steps 1-15).

# python: the reference system manager (obmc-phosphor-sysd) is its only user
IMAGE_FEATURES_remove = "obmc-system-mgmt"
# LDAP users, SNMP traps, telemetry, D-Bus monitor, health monitor, remote
# syslog, debug collector (dreport), devtools
IMAGE_FEATURES_remove = "obmc-user-mgmt-ldap obmc-telemetry obmc-dbus-monitor \
    obmc-health-monitor obmc-remote-logging-mgmt obmc-debug-collector obmc-devtools"
# legacy web UI (reimu-webui replaces it), debug and console tools, NFS
# automount, locale sources and tools (the compiled en_US/ru_RU.UTF-8 stay:
# tmux of the serial console needs a UTF-8 locale, shells have ru_RU.UTF-8)
OBMC_IMAGE_EXTRA_INSTALL_remove = "perf tcpdump ethtool phosphor-webui ipmitool \
    mc mc-fish mc-helpers mc-helpers-perl mc-locale-ru htop sysstat \
    autofs nfs-utils-client nss-pam-ldapd \
    glibc-localedata-en-us glibc-localedata-ru-ru glibc-utils"
# GNU wget with TLS (busybox wget stays)
OBMC_IMAGE_EXTRA_INSTALL_remove = "wget"
# the dual-chip reliable upgrade: this board has one chip, A/B replaces it
OBMC_IMAGE_EXTRA_INSTALL_remove = "reimu-bootguard"

# A/B updates (and the way back to the stock firmware)
OBMC_IMAGE_EXTRA_INSTALL_append = " reimu-ab"

# ---------------------------------------------------------------- A/B image
# The partitions (bytes) of reimu-ab64.dtsi; the build fails when a part
# doesn't fit, or leaves less than REIMU_AB_MARGIN_KB free in its partition.
REIMU_AB_LAYOUT = "u-boot:0x0:0xe0000 u-boot-env:0xe0000:0x20000 \
    kernel-a:0x100000:0x600000 rofs-a:0x700000:0x1500000 \
    kernel-b:0x1c00000:0x600000 rofs-b:0x2200000:0x1500000 rwfs:0x3700000:0x900000"
REIMU_AB_MARGIN_KB ?= "256"
REIMU_AB_ENV := "${THISDIR}/../../recipes-mcst/reimu-ab/files/reimu-ab.env"

DEPENDS += "u-boot-tools-native dtc-native"

# deploys:
#   reimu-ab-<ver>.tar   the update: image-kernel, image-rofs (rofs-X sized,
#                        digests in its last 4 KiB), SHA256SUMS, sizes
#   <name>.ab.mtd        the whole 64 MiB flash, both slots, an A/B
#                        environment (for QEMU; on the BMC U-Boot and its
#                        environment are never written)
python do_generate_ab() {
    import hashlib, os, struct, subprocess, zlib
    deploy = d.getVar('DEPLOY_DIR_IMAGE')
    imgdir = d.getVar('IMGDEPLOYDIR')
    layout = {}
    for p in d.getVar('REIMU_AB_LAYOUT').split():
        n, o, s = p.split(':')
        layout[n] = (int(o, 16), int(s, 16))
    margin = int(d.getVar('REIMU_AB_MARGIN_KB')) * 1024
    kernel = open(os.path.join(deploy, d.getVar('FLASH_KERNEL_IMAGE')), 'rb').read()
    rofs = open(os.path.join(imgdir, '%s.%s' % (d.getVar('IMAGE_LINK_NAME'),
                                                d.getVar('IMAGE_BASETYPE'))), 'rb').read()
    uboot = open(os.path.join(deploy, 'u-boot.%s' % d.getVar('UBOOT_SUFFIX')), 'rb').read()
    version = 'unknown'
    for l in open(os.path.join(d.getVar('IMAGE_ROOTFS'), 'usr/lib/os-release')):
        if l.startswith('VERSION_ID='):
            version = l.split('=', 1)[1].strip().strip('"')

    # both padded with 0xff to 4 KiB, as reimu-flash writes them: the digests
    # can then be checked with dd bs=4096 (the initramfs has no head -c)
    pad4k = lambda b: b + b'\xff' * (-len(b) % 4096)
    kernel, rofs = pad4k(kernel), pad4k(rofs)
    rofs_part = layout['rofs-a'][1]
    sizes = []
    for name, data, part in (('kernel', kernel, layout['kernel-a'][1]),
                             ('rofs', rofs, rofs_part - 4096),
                             ('u-boot', uboot, layout['u-boot'][1])):
        sizes.append('%-7s %9d of %9d bytes, %5d KiB free' %
                     (name, len(data), part, (part - len(data)) // 1024))
        if len(data) > part:
            bb.fatal('A/B: %s is %d bytes, its partition %d' % (name, len(data), part))
        if name != 'u-boot' and part - len(data) < margin:
            bb.fatal('A/B: %s leaves %d KiB in its partition, less than %d KiB'
                     % (name, (part - len(data)) // 1024, margin // 1024))
    bb.plain('A/B sizes:\n  ' + '\n  '.join(sizes))

    trailer = ('reimu-rofs %d %s\nreimu-kernel %d %s\nreimu-version %s\n' % (
        len(rofs), hashlib.sha256(rofs).hexdigest(),
        len(kernel), hashlib.sha256(kernel).hexdigest(), version)).encode()
    rofs_img = rofs + b'\xff' * (rofs_part - 4096 - len(rofs)) + \
        trailer + b'\xff' * (4096 - len(trailer))

    w = os.path.join(d.getVar('WORKDIR'), 'ab')
    os.makedirs(w, exist_ok=True)
    open(os.path.join(w, 'image-kernel'), 'wb').write(kernel)
    open(os.path.join(w, 'image-rofs'), 'wb').write(rofs_img)
    open(os.path.join(w, 'SHA256SUMS'), 'w').write(''.join(
        '%s  %s\n' % (hashlib.sha256(open(os.path.join(w, f), 'rb').read()).hexdigest(), f)
        for f in ('image-kernel', 'image-rofs')))
    open(os.path.join(w, 'SIZES'), 'w').write('\n'.join(sizes) + '\n')
    tar = os.path.join(imgdir, 'reimu-ab-%s.tar' % version)
    subprocess.check_call(['tar', '-cf', tar, '-C', w, 'image-kernel', 'image-rofs',
                           'SHA256SUMS', 'SIZES'])

    # the environment: the A/B script, test MACs, both redundant copies
    env = os.path.join(w, 'env.txt')
    with open(env, 'w') as f:
        f.write('bootdelay 2\nbaudrate 115200\nverify yes\nspi_dma yes\n')
        f.write('ethaddr 98:a7:b0:00:00:01\neth1addr 98:a7:b0:00:00:02\n')
        f.write('bootargs console=ttyS4,115200n8 root=/dev/ram rw\n')
        for l in open(d.getVar('REIMU_AB_ENV')):
            if l.strip() and not l.startswith('#'):
                f.write(l)
    # fw_setenv -s format (NAME VALUE) -> mkenvimage (NAME=VALUE)
    envtxt = os.path.join(w, 'env-mk.txt')
    with open(envtxt, 'w') as f:
        for l in open(env):
            n, _, v = l.rstrip('\n').partition(' ')
            f.write('%s=%s\n' % (n, v))
    envbin = os.path.join(w, 'env.bin')
    subprocess.check_call(['mkenvimage', '-r', '-s', '0x10000', '-o', envbin, envtxt])
    e1 = bytearray(open(envbin, 'rb').read())
    e2 = bytearray(e1); e1[4] = 1; e2[4] = 0  # copy 1 active

    flash = bytearray(b'\xff' * (int(d.getVar('FLASH_SIZE')) * 1024))
    def put(name, data):
        o, s = layout[name]
        assert len(data) <= s
        flash[o:o + len(data)] = data
    put('u-boot', uboot)
    put('u-boot-env', bytes(e1) + bytes(e2))
    for s in 'ab':
        put('kernel-' + s, kernel)
        put('rofs-' + s, rofs_img)
    name = d.getVar('IMAGE_NAME')
    open(os.path.join(imgdir, name + '.ab.mtd'), 'wb').write(flash)

    # the kexec bundle: this firmware started from RAM by another one (the
    # stock firmware) without touching the flash: for testing it on the board
    # (fdt-ro: every flash partition read-only) and for the first install
    def fit_images(fit):
        import struct
        be = lambda o: struct.unpack('>I', fit[o:o + 4])[0]
        ostruct, ostrings = be(8), be(12)
        p, depth, images, cur, out = ostruct, 0, False, {}, {}
        while p < len(fit):
            tok = be(p); p += 4
            if tok == 1:
                n = fit[p:fit.index(b'\0', p)].decode(); p += (len(n) + 4) & ~3
                depth += 1
                if depth == 2: images = n == 'images'
                if depth == 3: cur = {}
            elif tok == 2:
                if depth == 3 and images and 'data' in cur and 'type' in cur:
                    out[cur['type'].rstrip(b'\0').decode()] = cur['data']
                depth -= 1
            elif tok == 3:
                ln, no = be(p), be(p + 4)
                pn = fit[ostrings + no:fit.index(b'\0', ostrings + no)].decode()
                if depth == 3 and images and pn in ('data', 'type'):
                    cur[pn] = fit[p + 8:p + 8 + ln]
                p += 8 + ((ln + 3) & ~3)
            elif tok == 9:
                break
        return out
    def newc(name, data, mode=0o100644):
        n = name.encode() + b'\0'
        h = b'070701' + b''.join(b'%08X' % v for v in (1, mode, 0, 0, 1, 0, len(data), 0, 0, 0, 0, len(n), 0))
        pad = lambda b: b + b'\0' * (-len(b) % 4)
        return pad(h + n) + pad(data)
    imgs = fit_images(open(os.path.join(deploy, d.getVar('FLASH_KERNEL_IMAGE')), 'rb').read())
    kx = os.path.join(w, 'kexec'); os.makedirs(kx, exist_ok=True)
    open(os.path.join(kx, 'zImage'), 'wb').write(imgs['kernel'])
    open(os.path.join(kx, 'fdt'), 'wb').write(imgs['flat_dt'])
    rofs_raw = open(os.path.join(imgdir, '%s.%s' % (d.getVar('IMAGE_LINK_NAME'),
                                                    d.getVar('IMAGE_BASETYPE'))), 'rb').read()
    open(os.path.join(kx, 'initrd'), 'wb').write(imgs['ramdisk'] + newc('image-rofs', rofs_raw) +
                                                  newc('TRAILER!!!', b'', 0))
    dts = subprocess.check_output(['dtc', '-q', '-I', 'dtb', '-O', 'dts', os.path.join(kx, 'fdt')]).decode()
    import re
    # every partition but u-boot (read-only already); the flash node's own
    # label "bmc" too (harmless), the whole-chip partition "bmc", the PNS
    # flash's one partition "sys"
    ro, n = re.subn(r'(\n(\t+)label = "(u-boot-env|kernel-a|rofs-a|kernel-b|rofs-b|rwfs|bmc|sys)";)', r'\1\n\2read-only;', dts)
    if n != 9:
        bb.fatal('A/B: fdt-ro: %d flash labels made read-only, expected 9' % n)
    subprocess.run(['dtc', '-q', '-I', 'dts', '-O', 'dtb', '-o', os.path.join(kx, 'fdt-ro')],
                   input=ro.encode(), check=True)
    import shutil
    shutil.copy(os.path.join(d.getVar('IMAGE_ROOTFS'), 'usr/sbin/kexec'), os.path.join(kx, 'kexec'))
    ktar = os.path.join(imgdir, 'reimu-ab-kexec-%s.tar' % version)
    subprocess.check_call(['tar', '-cf', ktar, '-C', kx, 'zImage', 'fdt', 'fdt-ro', 'initrd', 'kexec'])
    for link, target in (('%s.ab.mtd' % d.getVar('IMAGE_LINK_NAME'), name + '.ab.mtd'),
                         ('reimu-ab.tar', os.path.basename(tar)),
                         ('reimu-ab-kexec.tar', os.path.basename(ktar))):
        p = os.path.join(imgdir, link)
        if os.path.lexists(p):
            os.remove(p)
        os.symlink(target, p)
}
do_generate_ab[depends] += "u-boot-tools-native:do_populate_sysroot dtc-native:do_populate_sysroot"
do_generate_ab[file-checksums] += "${REIMU_AB_ENV}:True"
addtask generate_ab after do_generate_static before do_image_complete
