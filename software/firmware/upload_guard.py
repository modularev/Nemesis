# SPDX-License-Identifier: GPL-3.0-or-later
# upload_guard.py - PlatformIO pre-upload check (macOS)
#
# The Teensy uploader programs whichever Teensy it finds, so with two units
# on USB a rev2 build once went to the rev1 unit. Before an upload this
# stops when more than one Teensy is connected, or when the connected one
# calls itself "Nemesis rev1"/"Nemesis rev2" and that isn't this build's
# NEMESIS_HW_REV (other names, e.g. a Teensy in its bootloader or older
# firmware, pass with a note).

import re
import subprocess

Import("env")  # noqa: F821 (PlatformIO)

TEENSY_VENDOR = 0x16C0


def connected_teensys():
    """(product name, product id) of every Teensy on USB."""
    try:
        out = subprocess.run(["ioreg", "-p", "IOUSB", "-w0", "-l"], capture_output=True, text=True, timeout=10).stdout
    except Exception:
        return None  # not macOS or ioreg missing: no check
    devices = []
    for block in re.split(r"\+-o ", out):
        vendor = re.search(r'"idVendor" = (\d+)', block)
        if not vendor or int(vendor.group(1)) != TEENSY_VENDOR:
            continue
        name = re.search(r'"USB Product Name" = "([^"]*)"', block)
        product = re.search(r'"idProduct" = (\d+)', block)
        devices.append((name.group(1) if name else "?", int(product.group(1)) if product else 0))
    return devices


def build_revision(env):
    for flag in env.get("CPPDEFINES", []):
        if isinstance(flag, (list, tuple)) and flag[0] == "NEMESIS_HW_REV":
            return int(flag[1])
    return None


def guard(source, target, env):
    devices = connected_teensys()
    if devices is None:
        return
    rev = build_revision(env)
    if len(devices) > 1:
        names = ", ".join(n for n, _ in devices)
        print("\n*** upload stopped: %d Teensys on USB (%s). Unplug all but the one to program.\n" % (len(devices), names))
        env.Exit(1)
    if len(devices) == 1 and rev is not None:
        name = devices[0][0]
        m = re.match(r"Nemesis rev(\d+)$", name)
        if m and int(m.group(1)) != rev:
            print("\n*** upload stopped: this is a rev%d build, the connected unit is \"%s\".\n" % (rev, name))
            env.Exit(1)
        if not m:
            print("upload_guard: the connected Teensy is \"%s\" (no revision in its name): uploading the rev%s build" % (name, rev))


env.AddPreAction("upload", guard)  # noqa: F821
