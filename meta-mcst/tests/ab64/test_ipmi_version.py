#!/usr/bin/env python3
"""ipmid's convertVersion with our patch over the version names a release gets:
the function out of the source of the build (the recipe's revision in the
download cache, the patch on it), compiled alone, the bytes as Get Device ID
makes them, printed as ipmitool does."""
import os, re, subprocess, sys, tempfile

TREE = "/work/openbmc"
GIT = "/work/downloads/git2/github.com.openbmc.phosphor-host-ipmid"
PATCH = TREE + "/meta-mcst/meta-ab64/recipes-phosphor/ipmi/phosphor-ipmi-host/0002-Get-Device-ID-firmware-revision-Get-System-GUID.patch"
CASES = [("reimu-1.0.2", "1.02"), ("reimu-1.0.2-66-g9e233e2504", "1.02"), ("reimu-1.0.2-dirty", "1.02"),
         ("reimu-1.0.2-66-g9e233e2504-dirty", "1.02"), ("reimu-2.0", "2.00"), ("reimu-2.0-beta", "2.00"),
         ("reimu-2.0-beta-dirty", "2.00"), ("reimu-2.0-beta-3-gabc1234", "2.00"),
         ("reimu-2.0-beta-3-gabc1234-dirty", "2.00"), ("reimu-2.1", "2.10"), ("reimu-2.1-rc1", "2.10"), ("reimu-2.1.5", "2.15")]
MAIN = r"""
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <regex>
#include <sstream>
#include <string>
struct Revision { char major; char minor; uint16_t d[2]; };
%s
int main(int c, char** v)
{
    for (int i = 1; i < c; i++)
    {
        Revision rev{};
        unsigned fw0 = 0, fw1 = 0;
        try
        {
            if (convertVersion(v[i], rev) >= 0)
            {
                fw0 = rev.major & 0x7f;
                unsigned char m = rev.minor > 99 ? 99 : rev.minor;
                fw1 = m %% 10 + (m / 10) * 16;
            }
        }
        catch (const std::exception&) {}
        printf("%%s %%x.%%02x\n", v[i], fw0, fw1);
    }
}
"""


def main():
    inc = open(TREE + "/meta-phosphor/recipes-phosphor/ipmi/phosphor-ipmi-host.inc").read()
    rev = re.search(r'^SRCREV\s*=\s*"(\w+)"', inc, re.M).group(1)
    with tempfile.TemporaryDirectory() as d:
        src = subprocess.check_output(["git", "-c", "safe.directory=*", "-C", GIT, "show", rev + ":apphandler.cpp"])
        open(d + "/apphandler.cpp", "wb").write(src)
        subprocess.check_call(["patch", "-s", "-p1", "-d", d, "-i", PATCH])
        s = open(d + "/apphandler.cpp").read()
        a = s.index("int convertVersion(std::string s, Revision& rev)")
        e = s.index("\n}\n", a) + 3
        open(d + "/t.cpp", "w").write(MAIN % s[a:e])
        subprocess.check_call(["g++", "-std=c++17", "-O0", "-o", d + "/t", d + "/t.cpp"])
        out = subprocess.check_output([d + "/t"] + [v for v, _ in CASES], text=True).splitlines()
    bad = 0
    for (v, want), l in zip(CASES, out):
        got = l.split()[1]
        ok = got == want
        bad += not ok
        print("  %s mc info Firmware Revision %s for %s (want %s)" % ("PASS" if ok else "FAIL", got, v, want))
    print("%d failed" % bad)
    sys.exit(1 if bad else 0)


main()
