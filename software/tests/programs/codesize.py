# SPDX-License-Identifier: GPL-3.0-or-later
#
# codesize.py <firmware.elf> <programs dir>: the size of every program's
# compute() in a built firmware. FAUST programs run from flash through the
# Teensy's 32 kB instruction cache (2-way); a compute() above ~30 kB
# evicts itself on every sample (strike at 40 kB: 57 -> 85 % CPU), unless
# the program runs from ITCM ('declare options "[itcm:on]"').
import os
import re
import subprocess
import sys

LIMIT = 30 * 1024
NM = os.path.expanduser("~/.platformio/packages/toolchain-gccarmnoneeabi-teensy/bin/arm-none-eabi-nm")

elf, programs = sys.argv[1], sys.argv[2]
sizes = {}
out = subprocess.run([NM, "-S", "-C", elf], capture_output=True, text=True).stdout
for line in out.splitlines():
    parts = line.split(None, 3)
    if len(parts) == 4 and parts[2] in "tTW" and re.search(r"::compute\(int", parts[3]):
        cls = parts[3].split("::compute")[0]
        sizes[cls] = sizes.get(cls, 0) + int(parts[1], 16)

itcm = set()
for f in os.listdir(programs):
    if f.endswith(".dsp") and "[itcm:on]" in open(os.path.join(programs, f), encoding="utf-8").read():
        itcm.add(f[:-4])

print("compute() sizes (%s):" % os.path.relpath(elf))
warn = 0
for cls in sorted(sizes):
    name = re.sub(r"_(dsp|effect)$", "", cls)
    note = ""
    if name in itcm:
        note = "  (ITCM)"
    elif sizes[cls] > LIMIT:
        note = "  warn: above %d kB, outgrows the instruction cache" % (LIMIT // 1024)
        warn += 1
    print("  %-22s %6d%s" % (cls, sizes[cls], note))
if warn:
    print("  %d program(s) above the instruction cache limit" % warn)
