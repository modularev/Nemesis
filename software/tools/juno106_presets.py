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
PWM bytes only reach 105 (R5/R6), the DCO LFO byte is after the slider's
taper: both converted to slider positions as Youth106 / KR-106 do.

usage: juno106_presets.py <patches.106|*.syx> [-o juno.PRE]
"""
import argparse
import re
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


# The factory order (A11..B88) with the names of the factory recordings at
# synthmania.com (Juno-106 Factory Preset Group A/B); a library whose names
# match all 128 is written in this order, numbered ("A11 Brass").
FACTORY = [
    ("A11", "Brass"),
    ("A12", "Brass Swell"),
    ("A13", "Trumpet"),
    ("A14", "Flutes"),
    ("A15", "Moving Strings"),
    ("A16", "Brass & Strings"),
    ("A17", "Choir"),
    ("A18", "Piano I"),
    ("A21", "Organ I"),
    ("A22", "Organ II"),
    ("A23", "Combo Organ"),
    ("A24", "Calliope"),
    ("A25", "Donald Pluck"),
    ("A26", "Celeste"),
    ("A27", "Elect. Piano I"),
    ("A28", "Elect. Piano II"),
    ("A31", "Clock Chimes"),
    ("A32", "Steel Drums"),
    ("A33", "Xylophone"),
    ("A34", "Brass III"),
    ("A35", "Fanfare"),
    ("A36", "Strings III"),
    ("A37", "Pizzicato"),
    ("A38", "High Strings"),
    ("A41", "Bass Clarinet"),
    ("A42", "English Horn"),
    ("A43", "Brass Ensemble"),
    ("A44", "Guitar"),
    ("A45", "Koto"),
    ("A46", "Dark Pluck"),
    ("A47", "Funky I"),
    ("A48", "Synth Bass I"),
    ("A51", "Lead I"),
    ("A52", "Lead II"),
    ("A53", "Lead III"),
    ("A54", "Funky II"),
    ("A55", "Synth Bass II"),
    ("A56", "Funky III"),
    ("A57", "Thud Wah"),
    ("A58", "Going Up"),
    ("A61", "Piano II"),
    ("A62", "Clav."),
    ("A63", "Frontier Organ"),
    ("A64", "Snare Drum"),
    ("A65", "Tom Toms"),
    ("A66", "Timpani"),
    ("A67", "Shaker"),
    ("A68", "Synth Pad"),
    ("A71", "Sweep I"),
    ("A72", "Pluck Sweep"),
    ("A73", "Repeater"),
    ("A74", "Sweep II"),
    ("A75", "Pluck Bell"),
    ("A76", "Dark Synth Piano"),
    ("A77", "Sustainer"),
    ("A78", "Wah Release"),
    ("A81", "Gong"),
    ("A82", "Resonance Funk"),
    ("A83", "Drum Booms"),
    ("A84", "Dust Storm"),
    ("A85", "Rocket Men"),
    ("A86", "Hand Claps"),
    ("A87", "FX Sweep"),
    ("A88", "Caverns"),
    ("B11", "Strings"),
    ("B12", "Violin"),
    ("B13", "Chorus Vibes"),
    ("B14", "Organ I"),
    ("B15", "Harpsichord I"),
    ("B16", "Recorder"),
    ("B17", "Perc. Pluck"),
    ("B18", "Noise Sweep"),
    ("B21", "Space Chimes"),
    ("B22", "Nylon Guitar"),
    ("B23", "Orchestral Pad"),
    ("B24", "Bright Pluck"),
    ("B25", "Organ Bell"),
    ("B26", "Accordion"),
    ("B27", "FX Rise 1"),
    ("B28", "FX Rise 2"),
    ("B31", "Brass"),
    ("B32", "Helicopter"),
    ("B33", "Lute"),
    ("B34", "Chorus Funk"),
    ("B35", "Tomita"),
    ("B36", "FX Sweep 1"),
    ("B37", "Sharp Reed"),
    ("B38", "Bass Pluck"),
    ("B41", "Resonant Rise"),
    ("B42", "Harpsichord 2"),
    ("B43", "Dark Ensemble"),
    ("B44", "Contact Wah"),
    ("B45", "Noise Sweep 2"),
    ("B46", "Glassy Wah"),
    ("B47", "Phase Ensemble"),
    ("B48", "Chorused Bell"),
    ("B51", "Clav"),
    ("B52", "Organ 2"),
    ("B53", "Bassoon"),
    ("B54", "Auto Release Noise Sweep"),
    ("B55", "Brass Ensemble"),
    ("B56", "Ethereal"),
    ("B57", "Chorus Bell 2"),
    ("B58", "Blizzard"),
    ("B61", "E. Piano with Tremolo"),
    ("B62", "Clarinet"),
    ("B63", "Thunder"),
    ("B64", "Reedy Organ"),
    ("B65", "Flute - Horn"),
    ("B66", "Toy Rhodes"),
    ("B67", "Surf's Up"),
    ("B68", "OW Bass"),
    ("B71", "Piccolo"),
    ("B72", "Melodic Taps"),
    ("B73", "Meow Bass"),
    ("B74", "Violin (high)"),
    ("B75", "High Bells"),
    ("B76", "Rolling Wah"),
    ("B77", "Ping Bell"),
    ("B78", "Brassy Organ"),
    ("B81", "Low Dark Strings"),
    ("B82", "Piccolo Trumpet"),
    ("B83", "Cello"),
    ("B84", "High Strings"),
    ("B85", "Rocket Men"),
    ("B86", "Forbidden Planet"),
    ("B87", "Froggy"),
    ("B88", "Owgan"),
]
# names in editor libraries that differ: (A)/(B) variants go to group A/B
ALIASES = {"String III": "A36", "Brass Set 1": "A11", "Brass": "B31", "Meow Brass": "B73",
           "Brass Ensemble (A)": "A43", "Brass Ensemble (B)": "B55", "Clav (A)": "A62", "Clav (B)": "B51",
           "High Strings (A)": "A38", "High Strings (B)": "B84", "Rocket Men (A)": "A85", "Rocket Men (B)": "B85"}


def simple(name):
    s = re.sub(r"\*?\s*\(.*?\)", "", name.lower())
    for roman, digit in (("iii", "3"), ("ii", "2"), ("i", "1")):
        s = re.sub(r"\b%s\b" % roman, digit, s)
    return re.sub(r"[^a-z0-9]", "", s)


def factory_order(patches):
    """The patches in factory order with numbered names, or None if they aren't the factory set."""
    left = list(patches)
    result = []
    for number, name in FACTORY:
        match = [p for p in left if ALIASES.get(p[0]) == number]
        if not match:  # the exact name first ("Organ I" and "Organ 1" are A21 and B14)
            match = [p for p in left if p[0] not in ALIASES and p[0] == name][:1]
        if not match:
            match = [p for p in left if p[0] not in ALIASES and simple(p[0]) == simple(name)]
        if len(match) != 1:
            return None
        left.remove(match[0])
        result.append(("%s %s" % (number, name), match[0][1]))
    return result


def dco_lfo_slider(t_post):
    """The DCO LFO byte is read after the slider's analog taper
    (V = x / (1 + 5x - 5x^2), a 50K pot with a 10K shunt): the slider
    position that gave it (as Youth106 / KR-106)."""
    if t_post <= 0:
        return 0.0
    if t_post >= 1:
        return 1.0
    a, b, c = 5 * t_post, 1 - 5 * t_post, -t_post
    return (-b + (b * b - 4 * a * c) ** 0.5) / (2 * a)


def convert(p):
    v = [x / 127 for x in p[:16]]
    sw1, sw2 = p[16], p[17]
    rng = {1: 0, 2: 1, 4: 2}.get(sw1 & 7, 1)               # 16', 8', 4'
    chorus_on = not (sw1 >> 5) & 1
    chorus = (1 if (sw1 >> 6) & 1 else 2) if chorus_on else 0          # juno's menu: off, I, II
    hpf_pos = 3 - ((sw2 >> 3) & 3)
    hpf = hpf_pos                                         # juno's menu: 0 boost, 1 flat, 2, 3
    values = [
        ("LFO Rate", v[0]), ("LFO Delay", v[1]), ("LFO DCO", dco_lfo_slider(p[2] / 127)),
        ("PWM", min(p[3], 105) / 105), ("Noise", v[4]),
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
    ordered = factory_order(patches)
    if ordered:
        patches = ordered
        print("factory patches: in the order A11 .. B88")
    with open(args.output, "w") as out:
        out.write("# juno presets: name|label=value|... (software/tools/juno106_presets.py)\n")
        for name, p in patches[:128]:
            name = "".join(" " if c in "'|:;}" else c for c in name).strip()
            out.write("%s|%s\n" % (name, convert(p)))
    print("%d patches -> %s" % (min(len(patches), 128), args.output))


if __name__ == "__main__":
    main()
