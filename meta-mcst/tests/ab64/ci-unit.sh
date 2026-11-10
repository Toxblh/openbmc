#!/bin/sh
# ci-unit.sh (in ubuntu:20.04, /work mounted as for ci-build.sh, after a
# finished build): the test suites that need the build's tree, run-all.py's
# container suites (test_static, test_ab_plan...), as root like `docker exec
# reimu` runs them at home (test_static unpacks the image's root-only files).
# The rest (QEMU, browser) is reimu-tests.yml.
set -eu
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq git python3 gcc g++ make patch curl qemu-user libc6-dev libfdt-dev squashfs-tools mtd-utils xz-utils \
    device-tree-compiler >/dev/null
git config --global --add safe.directory '*'
cd /work/openbmc
REIMU_DOCKER= REIMU_WORK=/tmp/w python3 meta-mcst/tests/ab64/run-all.py --only '^test_'
