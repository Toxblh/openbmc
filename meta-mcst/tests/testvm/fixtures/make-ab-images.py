#!/usr/bin/env python3
"""Builds test images for the reliable upgrade stand: two 64 MiB BMC flash
chips with the REIMU static layout (u-boot, u-boot-env, kernel, squashfs
rofs carrying /etc/os-release, rwfs) and a firmware update tar
(obmc-phosphor-image *.static.mtd.tar format: MANIFEST + image-*).

Usage: make-ab-images.py <outdir>     (needs mksquashfs)
"""
import hashlib, io, os, subprocess, sys, tarfile, tempfile

OUT = sys.argv[1]
os.makedirs(OUT, exist_ok=True)
PARTS = [("u-boot", 0x0, 0xE0000), ("u-boot-env", 0xE0000, 0x20000), ("kernel", 0x100000, 0x580000),
         ("rofs", 0x680000, 0x2980000), ("rwfs", 0x3000000, 0x1000000)]


def filler(seed, n):
    b = hashlib.sha256(seed.encode()).digest()
    return (b * (n // len(b) + 1))[:n]


def rofs(version):
    with tempfile.TemporaryDirectory() as d:
        os.makedirs(f"{d}/root/etc")
        open(f"{d}/root/etc/os-release", "w").write(
            f'ID="openbmc-phosphor"\nNAME="Phosphor OpenBMC (REIMU)"\nVERSION_ID="{version}"\n'
            f'PRETTY_NAME="REIMU 5564 {version}"\n')
        open(f"{d}/root/payload", "wb").write(filler("rofs" + version, 3 << 20))
        subprocess.run(["mksquashfs", f"{d}/root", f"{d}/r.sqsh", "-noappend", "-quiet", "-all-root"], check=True)
        return open(f"{d}/r.sqsh", "rb").read()


def parts(version):
    return {"u-boot": filler("uboot" + version, 300 << 10), "u-boot-env": b"bootdelay=2\0\0",
            "kernel": filler("kernel" + version, 3 << 20), "rofs": rofs(version), "rwfs": b""}


def chip(path, version, rw_seed):
    img = bytearray(b"\xff" * (64 << 20))
    for name, off, size in PARTS:
        data = parts(version)[name] if name != "rwfs" else filler(rw_seed, 1 << 20)
        assert len(data) <= size, name
        img[off:off + len(data)] = data
    open(path, "wb").write(img)


chip(f"{OUT}/chipA.img", "2.0.0", "rwfs-A")
chip(f"{OUT}/chipB.img", "1.9.0", "rwfs-B")

for version, name in (("2.1.0", "fw-2.1.0.tar"), ("2.2.0-broken", "fw-2.2.0-broken.tar")):
    with tarfile.open(f"{OUT}/{name}", "w", format=tarfile.GNU_FORMAT) as t:
        files = {"MANIFEST": f"purpose=xyz.openbmc_project.Software.Version.VersionPurpose.BMC\nversion={version}\nMachineName=reimu-5564\n".encode()}
        for n, data in parts(version).items():
            if n != "u-boot-env":
                files[f"image-{n}"] = data
        for n, data in files.items():
            ti = tarfile.TarInfo(n)
            ti.size = len(data)
            t.addfile(ti, io.BytesIO(data))
print("images in", OUT)
