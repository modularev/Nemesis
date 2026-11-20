# SPDX-License-Identifier: GPL-3.0-or-later
#
# module.py - every program on a connected Nemesis: CPU load, peaks, late
# audio blocks, NaN resets, the free heap while each runs and after
# switching through all (back at the program running at the start).
# The load depends on the knob-bound values (the physical knobs): each
# result keeps them, and a CPU rise with knobs moved since the baseline is
# reported as such, not as a regression.
#
#   tests/.venv/bin/python tests/programs/module.py            measure, compare with the baseline
#   tests/.venv/bin/python tests/programs/module.py --save     measure and store as the new baseline
#   ... --secs 10 --port /dev/cu.usbmodemXXXX --only juno,pollen
#
# Each program runs --secs seconds after a 3 s settling time (its start
# and the switch show up in the first report). The baseline is per hardware
# revision (baseline_rev1.json / baseline_rev2.json next to this script);
# the comparison flags CPU 3 points above it, a peak above 95 %, late blocks,
# NaN resets and 16 kB less heap. Close the remote page and serial monitors
# first: only one program can have the port open.
import argparse
import glob
import json
import os
import re
import sys
import time

import serial

HERE = os.path.dirname(os.path.abspath(__file__))
CPU = re.compile(r"PROGRAM (\d+) (\S+)\s+CPU ([\d.]+)% \(max ([\d.]+)%\)")
LATE = re.compile(r"output written late (\d+) of (\d+) blocks")
NAN = re.compile(r"(\d+) resets after inf/NaN output")
HEAP = re.compile(r"heap free (\d+) kB")


class Module:
    def __init__(self, port):
        self.s = serial.Serial(port, 115200, timeout=0.2)
        time.sleep(0.5)
        self.s.reset_input_buffer()

    def lines(self, secs):
        end = time.time() + secs
        while time.time() < end:
            line = self.s.readline().decode(errors="replace").strip()
            if line:
                yield line

    def ask(self, command, pattern, secs=3.0):
        self.s.write((command + "\n").encode())
        for line in self.lines(secs):
            m = pattern.search(line)
            if m:
                return m, line
        return None, None


def find_port(port):
    if port:
        return port
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    if len(ports) != 1:
        sys.exit("found %d Nemesis ports (%s): give one with --port" % (len(ports), ", ".join(ports) or "none"))
    return ports[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--secs", type=float, default=8.0)
    ap.add_argument("--only", help="program names, comma separated")
    ap.add_argument("--save", action="store_true", help="store the results as the baseline")
    args = ap.parse_args()

    m = Module(find_port(args.port))
    match, line = m.ask("JSON", re.compile(r"^JSON (.*)$"))
    if not match:
        sys.exit("no answer to JSON: is the firmware running and the port free?")
    state = json.loads(match.group(1))
    rev, programs, start = state["rev"], state["programs"], state["current"]
    only = set(args.only.split(",")) if args.only else None
    print("Nemesis rev%d, %d programs, %.0f s each" % (rev, len(programs), args.secs))

    results = {}
    for n, name in enumerate(programs):
        if only and name not in only:
            continue
        m.s.write(("PROG %d\n" % (n + 1)).encode())
        list(m.lines(3.0))   # settling: the start and the switch
        before, _ = m.ask("STATUS", LATE)   # late blocks counted from here
        cpu, peak = [], 0.0
        for l in m.lines(args.secs):
            c = CPU.search(l)
            if c and c.group(2) == name:
                cpu.append(float(c.group(3)))
                peak = max(peak, float(c.group(4)))
        m.s.write(b"STATUS\n")
        late, nan = None, 0
        for l in m.lines(3.0):
            x = LATE.search(l)
            if x:
                late = int(x.group(1)) - (int(before.group(1)) if before else 0)
            y = NAN.search(l)
            if y:
                nan = int(y.group(1))
            if x and late is not None:
                break
        h, _ = m.ask("MEM", HEAP)
        # the knob-bound values: the load depends on them (pollen 59 % with
        # density and pitch in the middle, 78 % at full), and they are
        # wherever the physical knobs are
        js, _ = m.ask("JSON", re.compile(r"^JSON (.*)$"))
        knobs = {}
        if js:
            for c in json.loads(js.group(1)).get("controls", []):
                if c.get("input"):
                    knobs[c["label"]] = round(c["value"], 2)
        r = {"cpu": round(sum(cpu) / len(cpu), 1) if cpu else None, "peak": peak, "late": late, "nan": nan,
             "heap_kb": int(h.group(1)) if h else None, "knobs": knobs}
        results[name] = r
        print("  %-11s CPU %5s %%  peak %5.1f %%  late %s  NaN resets %d  heap free %s kB" %
              (name, r["cpu"], peak, "?" if late is None else late, nan, r["heap_kb"]), flush=True)

    # the heap after all programs, back at the one running at the start (a
    # program's own buffers in RAM2 count while it runs)
    m.s.write(("PROG %d\n" % (start + 1)).encode())
    list(m.lines(2.5))
    heap, _ = m.ask("MEM", HEAP)
    heap_kb = int(heap.group(1)) if heap else None
    print("  heap free after all programs, back at %s: %s kB" % (programs[start], heap_kb))

    base_file = os.path.join(HERE, "baseline_rev%d.json" % rev)
    if args.save:
        with open(base_file, "w") as f:
            json.dump({"date": time.strftime("%Y-%m-%d"), "programs": results, "heap_kb": heap_kb,
                       "start": programs[start]}, f, indent=1)
        print("saved %s" % os.path.relpath(base_file))
        return

    problems = []
    for name, r in results.items():
        if r["late"]:
            problems.append("%s: %d late blocks" % (name, r["late"]))
        if r["nan"]:
            problems.append("%s: %d NaN resets" % (name, r["nan"]))
        if r["peak"] > 95:
            problems.append("%s: peak %.1f %%" % (name, r["peak"]))
    if os.path.exists(base_file):
        base = json.load(open(base_file))
        print("compared with %s (%s)" % (os.path.basename(base_file), base.get("date")))
        for name, r in results.items():
            b = base["programs"].get(name)
            if b and b["cpu"] is not None and r["cpu"] is not None and r["cpu"] > b["cpu"] + 3:
                moved = [k for k, v in r.get("knobs", {}).items()
                         if k in b.get("knobs", {}) and abs(v - b["knobs"][k]) > 0.05]
                if moved:
                    print("  %s: CPU %.1f %% (baseline %.1f %%) with knobs moved since the baseline: %s" %
                          (name, r["cpu"], b["cpu"], ", ".join("%s %.2f (was %.2f)" % (k, r["knobs"][k], b["knobs"][k]) for k in moved)))
                elif "knobs" not in b:
                    problems.append("%s: CPU %.1f %% (baseline %.1f %%; the baseline has no knob positions)" % (name, r["cpu"], b["cpu"]))
                else:
                    problems.append("%s: CPU %.1f %% (baseline %.1f %%)" % (name, r["cpu"], b["cpu"]))
        if heap_kb is not None and base.get("heap_kb") and base.get("start") == programs[start] \
                and heap_kb < base["heap_kb"] - 16:
            problems.append("heap %d kB (baseline %d kB)" % (heap_kb, base["heap_kb"]))
    else:
        print("no baseline yet for rev%d (--save stores one)" % rev)
    for p in problems:
        print("  CHECK " + p)
    print("ok" if not problems else "%d to check" % len(problems))
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
