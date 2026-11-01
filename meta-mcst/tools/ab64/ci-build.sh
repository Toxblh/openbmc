#!/bin/sh
# ci-build.sh (as root in ubuntu:20.04, /work mounted): what the local build
# container has, then the image, as UID_OUT. Exit 124: out of time, go on later.
set -eu
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq git curl cpio wget python3 python3-dev python3-distutils python3-pip \
    build-essential gcc g++ make gawk diffstat chrpath binutils libfdt1 libfdt-dev locales file texinfo \
    zstd lz4 xz-utils bzip2 unzip socat iputils-ping sudo vim less rsync squashfs-tools mtd-utils \
    dpkg-dev binutils-arm-linux-gnueabi cmake python3-venv >/dev/null
sed -i 's/^# *en_US.UTF-8/en_US.UTF-8/' /etc/locale.gen; locale-gen >/dev/null
groupadd -g "$GID_OUT" builder 2>/dev/null || true
useradd -m -u "$UID_OUT" -g "$GID_OUT" builder
chown "$UID_OUT:$GID_OUT" /work
# stop in time for the sstate to be saved: a job has 6 h
cat > /tmp/ci-inner.sh <<'E'
#!/bin/bash
git config --global --add safe.directory '*'
cd /work/openbmc && source setup reimu-5564 /work/build-ab >/dev/null || exit 1
grep -q '^REIMU_CI' conf/local.conf || cat >> conf/local.conf <<'C'
REIMU_CI = "1"
DL_DIR = "/work/downloads"
SSTATE_DIR = "/work/sstate"
BB_NUMBER_THREADS = "4"
PARALLEL_MAKE = "-j 4"
INHERIT += "rm_work"
RM_WORK_EXCLUDE += "obmc-phosphor-image"
C
timeout -k 600 "${CI_BUILD_SECONDS:-18600}" bitbake -k obmc-phosphor-image
E
chmod 755 /tmp/ci-inner.sh
exec su builder -c 'LANG=en_US.UTF-8 LC_ALL=en_US.UTF-8 /tmp/ci-inner.sh'
