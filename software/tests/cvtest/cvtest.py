#!/usr/bin/env python3
"""
CV input test for the NEMESIS rev2, using DC coupled audio interface outputs
(e.g. an Expert Sleepers ES-10 on ADAT) as voltage source. Needs the
tests/hwtest firmware on the module and: pip install sounddevice numpy pyserial

1. Measure the output scale, nothing patched to the module yet:

     python3 cvtest.py hold 0.1          # all 8 outputs at 0.1 of full scale
     python3 cvtest.py hold 0.5

   Measure the voltage of each output with a multimeter. volts per unit is
   (V at 0.5 - V at 0.1) / 0.4, offset is the voltage at 0.

2. Patch output 1..7 to CV X Y Z A B C D (X/Y/Z attenuverters fully
   clockwise) and sweep:

     python3 cvtest.py sweep --volts-per-unit 10.0

   Every input is driven on its own from --min-volts to --max-volts while all
   inputs are read, which gives gain, offset, linearity, channel order and
   crosstalk, saved to cv_fit.json. CV X/Y/Z are bipolar (about -4.2..+4.2 V):

     python3 cvtest.py sweep --volts-per-unit 11.046 --offset -0.013 \\
         --inputs CV_X,CV_Y,CV_Z --min-volts -5 --max-volts 5 --step 0.5

3. Module outputs, measured with the module's own CV inputs (loopback): patch
   output 1..7 to CV X Y Z A B C D and run

     python3 cvtest.py dac

   Every output steps through DC levels; a reading that decays back to 0 V
   within a few seconds means the output is AC coupled.
"""

import argparse
import json
import os
import sys
import threading
import time

import numpy as np
import serial
import sounddevice as sd

CV_NAMES = ["CV_X", "CV_Y", "CV_Z", "CV_A", "CV_B", "CV_C", "CV_D"]
FIT_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "cv_fit.json")


class DCOutput:
    """Constant values on a block of output channels of an audio interface."""

    def __init__(self, device, first_output, count):
        info = sd.query_devices(device, "output")
        self.channels = info["max_output_channels"]
        self.first = first_output - 1
        self.count = count
        if self.first + count > self.channels:
            sys.exit("device has only %d outputs" % self.channels)
        self.values = np.zeros(self.channels, dtype=np.float32)
        self.stream = sd.OutputStream(device=info["index"], channels=self.channels,
                                      dtype="float32", callback=self._callback)
        print("output: %s, channels %d-%d" % (info["name"], first_output, first_output + count - 1))

    def _callback(self, outdata, frames, time_info, status):
        outdata[:] = self.values

    def set(self, values):
        v = np.zeros(self.channels, dtype=np.float32)
        v[self.first:self.first + self.count] = values
        self.values = v

    def __enter__(self):
        self.set(np.zeros(self.count))
        self.stream.start()
        return self

    def __exit__(self, *exc):
        self.set(np.zeros(self.count))
        time.sleep(0.2)
        self.stream.stop()
        self.stream.close()


class Nemesis:
    """Reads the RAW lines of the hwtest firmware in the background."""

    def __init__(self, port):
        self.serial = serial.Serial(port, 115200, timeout=1)
        self.samples = []
        self.lock = threading.Lock()
        self.running = True
        self.thread = threading.Thread(target=self._read, daemon=True)
        self.thread.start()

    def _read(self):
        while self.running:
            try:
                line = self.serial.readline().decode(errors="replace")
            except (serial.SerialException, OSError, TypeError):
                return  # port closed
            if line.startswith("RAW"):
                values = [float(v) for v in line.split()[1:]]
                with self.lock:
                    self.samples.append((time.time(), values))

    def average(self, seconds):
        start = time.time()
        time.sleep(seconds)
        with self.lock:
            window = [v for t, v in self.samples if t >= start]
            self.samples = self.samples[-100:]
        if not window:
            sys.exit("no data from the module, is the hwtest firmware running?")
        return np.mean(np.array(window)[:, :len(CV_NAMES)], axis=0)

    def send(self, line):
        self.serial.write((line + "\n").encode())

    def close(self):
        self.running = False
        self.serial.close()


def load_fit():
    with open(FIT_FILE) as f:
        return json.load(f)


def to_volts(fit, name, reading):
    return (reading - fit[name]["zero"]) / fit[name]["per_volt"]


def hold(args):
    with DCOutput(args.device, args.first_output, 8) as out:
        out.set(np.full(8, args.value))
        print("holding %.4f on all 8 outputs for %d s, measure now (ctrl-c to stop)" % (args.value, args.seconds))
        try:
            time.sleep(args.seconds)
        except KeyboardInterrupt:
            pass


def sweep(args):
    volts = np.arange(args.min_volts, args.max_volts + 1e-9, args.step)
    units = (volts - args.offset) / args.volts_per_unit
    if np.any(np.abs(units) > 1.0):
        sys.exit("--min-volts/--max-volts are beyond the output range")
    channels = [CV_NAMES.index(n) for n in args.inputs.split(",")]

    nemesis = Nemesis(args.port)
    results = np.zeros((len(CV_NAMES), len(volts), len(CV_NAMES)))  # driven, step, read
    with DCOutput(args.device, args.first_output, 8) as out:
        for ch in channels:
            name = CV_NAMES[ch]
            print("driving %s from output %d" % (name, args.first_output + ch))
            for i, u in enumerate(units):
                level = np.full(8, -args.offset / args.volts_per_unit)
                level[ch] = u
                out.set(level)
                time.sleep(args.settle)
                results[ch, i] = nemesis.average(args.average)
                print("  %5.2f V -> %.5f" % (volts[i], results[ch, i, ch]))
    nemesis.close()

    fit = load_fit() if os.path.exists(FIT_FILE) else {}

    # readings at the ends of the ADC range are clipped and left out of the fit
    print("\ninput  per volt   reads 0 at  reads 1 at  0 V reads  nonlinearity  crosstalk  responds on  (points)")
    for ch in channels:
        name = CV_NAMES[ch]
        readings = results[ch, :, ch]
        valid = (readings > 0.002) & (readings < 0.998)
        if valid.sum() < 2:
            print("%-6s not enough unclipped points" % name)
            continue
        slope, intercept = np.polyfit(volts[valid], readings[valid], 1)
        residual = readings[valid] - (slope * volts[valid] + intercept)
        nonlinearity_mv = np.max(np.abs(residual)) / abs(slope) * 1000
        others = np.delete(results[ch], ch, axis=1)
        crosstalk = np.max(np.abs(others - others[0]))
        strongest = CV_NAMES[int(np.argmax(np.abs(results[ch, -1] - results[ch, 0])))]
        print("%-6s %8.5f   %7.3f V   %7.3f V   %8.5f   %8.1f mV   %8.5f   %-11s  (%d of %d)" % (
            name, slope, -intercept / slope, (1 - intercept) / slope, intercept,
            nonlinearity_mv, crosstalk, strongest, valid.sum(), len(volts)))
        fit[name] = {"per_volt": round(slope, 6), "zero": round(intercept, 6)}

    with open(FIT_FILE, "w") as f:
        json.dump(fit, f, indent=2)
    print("saved to %s" % FIT_FILE)


def dac(args):
    fit = load_fit()
    patch = [p.split(":") for p in args.patch.split(",")]
    levels = [float(v) for v in args.levels.split(",")]
    nemesis = Nemesis(args.port)
    time.sleep(0.5)
    for output, _ in patch:
        nemesis.send("DC %d 0" % int(output))

    results = {}
    for output, cv in patch:
        output = int(output)
        ch = CV_NAMES.index(cv)
        print("output %d -> %s" % (output + 1, cv))
        rows = []
        for level in levels:
            nemesis.send("DC %d %.4f" % (output, level))
            time.sleep(0.15)
            early = nemesis.average(0.3)[ch]
            time.sleep(args.hold)
            late = nemesis.average(0.5)[ch]
            clipped = not (0.002 < late < 0.998)
            v_early, v_late = to_volts(fit, cv, early), to_volts(fit, cv, late)
            rows.append((level, v_late, clipped))
            print("  level %+.3f: %+7.3f V right after the step, %+7.3f V after %.0f s%s" % (
                level, v_early, v_late, args.hold, "  (clipped)" if clipped else ""))
        nemesis.send("DC %d 0" % output)
        results[output] = rows
    nemesis.close()

    print("\noutput  volts per unit  offset   coupling")
    for output, rows in results.items():
        valid = [(l, v) for l, v, c in rows if not c]
        if len(valid) < 2:
            print("%6d  not enough unclipped levels" % (output + 1))
            continue
        lv = np.array(valid)
        slope, offset = np.polyfit(lv[:, 0], lv[:, 1], 1)
        print("%6d  %12.3f V  %+6.3f V  %s" % (output + 1, slope, offset,
                                               "DC" if abs(slope) > 1.0 else "AC or not connected"))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--device", default="Fireface", help="audio output device (name or part of it)")
    parser.add_argument("--first-output", type=int, default=23, help="first DC coupled output (1 based)")
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("hold", help="hold a constant level on all 8 outputs")
    p.add_argument("value", type=float, help="level as fraction of full scale, -1..1")
    p.add_argument("--seconds", type=int, default=60)
    p.set_defaults(func=hold)

    p = sub.add_parser("sweep", help="measure all CV inputs")
    p.add_argument("--volts-per-unit", type=float, required=True, help="measured volts at full scale")
    p.add_argument("--offset", type=float, default=0.0, help="measured volts at level 0")
    p.add_argument("--min-volts", type=float, default=0.0)
    p.add_argument("--max-volts", type=float, default=8.0)
    p.add_argument("--inputs", default=",".join(CV_NAMES), help="comma separated, e.g. CV_X,CV_Y")
    p.add_argument("--step", type=float, default=1.0)
    p.add_argument("--settle", type=float, default=0.4)
    p.add_argument("--average", type=float, default=0.5)
    p.add_argument("--port", default="/dev/cu.usbmodem103226001")
    p.set_defaults(func=sweep)

    p = sub.add_parser("dac", help="measure the module outputs with its own CV inputs")
    p.add_argument("--patch", default="0:CV_X,1:CV_Y,2:CV_Z,3:CV_A,4:CV_B,5:CV_C,6:CV_D",
                   help="output index (0 based):CV input pairs")
    p.add_argument("--levels", default="0,0.05,0.1,0.2,-0.05,-0.1,-0.2")
    p.add_argument("--hold", type=float, default=3.0, help="seconds before the second reading")
    p.add_argument("--port", default="/dev/cu.usbmodem103226001")
    p.set_defaults(func=dac)

    args = parser.parse_args()
    if args.command == "hold" and abs(args.value) > 1:
        sys.exit("value must be within -1..1")
    args.func(args)


if __name__ == "__main__":
    main()
