#!/usr/bin/env python3
"""Juno-106 patches -> presets for the NEMESIS program juno.

Reads a patch library saved by a Juno-106 editor (Java serialized, .106:
patch names with their 18 data bytes) or Juno-106 SysEx dumps (.syx: F0 41
30 <ch> <src> + 18 bytes + F7, one patch each, named "Patch N") and writes
juno.PRE: one line per patch, "name|label=value|..." with the values of
juno's settings (nemesis_settings.cpp, settings::loadPresets). Copy it to
/NEMESIS/juno.PRE on the SD card (or the firmware's serial command WRITE).

Patch bytes (Juno-106 owner's manual, as quoted in ST-Log 33, June 1989):
 0 LFO rate  1 LFO delay  2 DCO LFO  3 DCO PWM  4 noise  5 VCF freq  6 VCF res
 7 VCF env  8 VCF LFO  9 VCF kybd  10 VCA level  11 attack  12 decay
 13 sustain  14 release  15 sub (0..127 each)
 16 switches 1: bit 0..2 range 16'/8'/4', 3 pulse, 4 saw, 5 chorus off,
    6 chorus I (else II)
 17 switches 2: bit 0 PWM manual, 1 env polarity -, 2 VCA gate,
    3..4 HPF (11 = position 0 .. 00 = position 3)
(also the MIDI implementation in the Juno-106 owner's manual, 3.4)

usage: juno106_presets.py <patches.106|*.syx> [-o juno.PRE]
"""
import argparse
import struct
import sys


def read_106(data):
    """(name, 18 bytes) from a Java serialized patch library."""
    patches, name, i = [], None, 0
    while i < len(data):
        b = data[i]
        if b == 0x74 and i + 3 <= len(data):             # TC_STRING
            n = struct.unpack(">H", data[i + 1:i + 3])[0]
            s = data[i + 3:i + 3 + n]
            if 0 < n < 64 and all(32 <= c < 127 for c in s):
                name = s.decode()
                i += 3 + n
                continue
        if b == 0x75 and name:                            # TC_ARRAY of 18 bytes
            j = data.find(b"\x00\x00\x00\x12", i, i + 64)
            if j > 0:
                patches.append((name, list(data[j + 4:j + 22])))
                name = None
                i = j + 22
                continue
        i += 1
    return patches


def read_syx(data):
    patches, i = [], 0
    while True:
        i = data.find(b"\xf0\x41\x30", i)
        if i < 0 or i + 24 > len(data):
            return patches
        patches.append(("Patch %d" % (len(patches) + 1), list(data[i + 5:i + 23])))
        i += 24


def convert(p):
    v = [x / 127 for x in p[:16]]
    sw1, sw2 = p[16], p[17]
    rng = {1: 0, 2: 1, 4: 2}.get(sw1 & 7, 1)               # 16', 8', 4'
    chorus_on = not (sw1 >> 5) & 1
    chorus = (1 if (sw1 >> 6) & 1 else 2) if chorus_on else 0          # juno's menu: off, I, II
    hpf_pos = 3 - ((sw2 >> 3) & 3)
    hpf = hpf_pos                                         # juno's menu: 0 boost, 1 flat, 2, 3
    values = [
        ("LFO Rate", v[0]), ("LFO Delay", v[1]), ("LFO DCO", v[2]), ("PWM", v[3]), ("Noise", v[4]),
        ("Cutoff", v[5]), ("Resonance", v[6]), ("Env", v[7]), ("LFO VCF", v[8]), ("Kybd", v[9]),
        ("Level", v[10]), ("Attack", v[11]), ("Decay", v[12]), ("Sustain", v[13]), ("Release", v[14]),
        ("Sub", v[15]), ("range", rng), ("Pulse", (sw1 >> 3) & 1), ("Saw", (sw1 >> 4) & 1),
        ("Chorus", chorus), ("PWM mode", sw2 & 1), ("env polarity", (sw2 >> 1) & 1),
        ("VCA", (sw2 >> 2) & 1), ("HPF", hpf),
    ]
    return "|".join("%s=%s" % (k, ("%.3f" % x).rstrip("0").rstrip(".") if isinstance(x, float) else x)
                    for k, x in values)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("files", nargs="+")
    ap.add_argument("-o", "--output", default="juno.PRE")
    args = ap.parse_args()
    patches = []
    for f in args.files:
        data = open(f, "rb").read()
        patches += read_106(data) if data[:2] == b"\xac\xed" else read_syx(data)
    if not patches:
        sys.exit("no patches found")
    with open(args.output, "w") as out:
        out.write("# juno presets: name|label=value|... (software/tools/juno106_presets.py)\n")
        for name, p in patches[:128]:
            name = "".join(" " if c in "'|:;}" else c for c in name).strip()
            out.write("%s|%s\n" % (name, convert(p)))
    print("%d patches -> %s" % (min(len(patches), 128), args.output))


if __name__ == "__main__":
    main()
