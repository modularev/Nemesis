#!/usr/bin/env python3
"""
CV input test for the NEMESIS rev2, using DC coupled audio interface outputs
(e.g. an Expert Sleepers ES-10 on ADAT) as voltage source. Needs the
tests/hwtest firmware on the module. Run it with tests/.venv/bin/python
(see tests/requirements.txt).

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

4. Calibration, stored in the module's EEPROM:

     python3 cvtest.py calibrate-inputs     # step 2 patch, X/Y/Z main knobs
                                            # fully counter-clockwise
     python3 cvtest.py calibrate-outputs    # patch output 1..4 to CV A..D,
     python3 cvtest.py calibrate-outputs --patch 4:CV_A,5:CV_B,6:CV_C,7:CV_D

   The ES-10 defaults (--es10-*) are the multimeter readings of this setup.
   X/Y/Z only get their gain calibrated, their zero is set by the main knob.

5. Outputs against an HP 34401A over RS-232 (driver in ~/GitHub/34401a-calibrator,
   installed in tests/.venv): meter on output N (tip HI, sleeve LO), then

     python3 cvtest.py calibrate-outputs-dmm --output N

6. CV inputs from the calibrated outputs (loopback), output 1..7 patched to
   CV X Y Z A B C D, X/Y/Z main knobs around noon:

     python3 cvtest.py calibrate-inputs-loopback

7. Audio inputs (codec ADC), output 1..8 patched to audio input 1..8:

     python3 cvtest.py audio-inputs
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
BIPOLAR = ("CV_X", "CV_Y", "CV_Z")
OUTPUT_VOLTS = 5.0  # module outputs per unit after calibration
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
        self.ain = []
        self.replies = []
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
            elif line.startswith("AIN"):
                values = [float(v) for v in line.split()[1:]]
                with self.lock:
                    self.ain.append((time.time(), values))
            elif line.startswith("OK") or line.startswith("OUT") or line.startswith("CV_"):
                self.replies.append(line.strip())

    def average(self, seconds):
        start = time.time()
        time.sleep(seconds)
        with self.lock:
            window = [v for t, v in self.samples if t >= start]
            self.samples = self.samples[-100:]
        if not window:
            sys.exit("no data from the module, is the hwtest firmware running?")
        return np.mean(np.array(window)[:, :len(CV_NAMES)], axis=0)

    def average_ain(self, seconds):
        """Mean of the audio input meters: (dc[8], rms[8]) in digital full scale."""
        start = time.time()
        time.sleep(seconds)
        with self.lock:
            window = [v for t, v in self.ain if t >= start]
            self.ain = self.ain[-100:]
        if not window:
            sys.exit("no AIN data, is the current hwtest firmware running?")
        a = np.mean(np.array(window), axis=0)
        return a[:8], a[8:16]

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


class ES10:
    """Volts to output levels, from multimeter readings of every output."""

    def __init__(self, args):
        self.offset = args.es10_offset
        half = [float(v) for v in args.es10_volts_at_half.split(",")]
        self.per_unit = [(v - self.offset) / 0.5 for v in half]

    def level(self, output, volts):
        u = (volts - self.offset) / self.per_unit[output]
        if abs(u) > 1.0:
            sys.exit("%.2f V is beyond the range of output %d" % (volts, output + 1))
        return u

    def levels(self, volts):
        return np.array([self.level(o, volts) for o in range(8)])


def add_es10_args(p):
    p.add_argument("--es10-offset", type=float, default=-0.013, help="volts at level 0")
    p.add_argument("--es10-volts-at-half", default="5.51,5.496,5.497,5.525,5.498,5.505,5.512,5.499",
                   help="multimeter reading of every output at level 0.5")


def calibrate_inputs(args):
    es10 = ES10(args)
    volts = np.arange(args.min_volts, args.max_volts + 1e-9, args.step)
    nemesis = Nemesis(args.port)
    fit = load_fit() if os.path.exists(FIT_FILE) else {}
    with DCOutput(args.device, args.first_output, 8) as out:
        out.set(es10.levels(0.0))
        for ch, name in enumerate(CV_NAMES):
            readings = []
            for v in volts:
                level = es10.levels(0.0)
                level[ch] = es10.level(ch, v)
                out.set(level)
                time.sleep(args.settle)
                readings.append(nemesis.average(args.average)[ch])
            readings = np.array(readings)
            valid = (readings > 0.002) & (readings < 0.998)
            if valid.sum() < 6:
                sys.exit("%s: only %d unclipped points, check the patch (X/Y/Z main knob fully ccw)" % (name, valid.sum()))
            slope, intercept = np.polyfit(volts[valid], readings[valid], 1)
            nonlinearity_mv = np.max(np.abs(readings[valid] - (slope * volts[valid] + intercept))) / slope * 1000
            zero = 0.5 if name in BIPOLAR else intercept  # X/Y/Z: the main knob sets the zero
            fit[name] = {"per_volt": round(slope, 6), "zero": round(intercept, 6)}
            nemesis.send("CVCAL %d %.6f %.6f" % (ch, zero, slope))
            print("%-6s %.5f per volt (design 0.12000, %+.2f%%), 0 V reads %.5f, linear to %.1f mV (%d points)" % (
                name, slope, (slope / 0.12 - 1) * 100, intercept, nonlinearity_mv, valid.sum()))

        nemesis.send("CALSTORE")
        time.sleep(0.5)
        # check: A-D at a few voltages, with the stored calibration
        print("\ncheck, calibrated readings of CV A-D:")
        for ch in range(3, 7):
            name = CV_NAMES[ch]
            errors = []
            for v in (1.0, 4.0, 7.0):
                level = es10.levels(0.0)
                level[ch] = es10.level(ch, v)
                out.set(level)
                time.sleep(args.settle)
                r = nemesis.average(args.average)[ch]
                errors.append((r - fit[name]["zero"]) / fit[name]["per_volt"] - v)
            print("  %s error at 1/4/7 V: %s mV" % (name, " / ".join("%+.1f" % (e * 1000) for e in errors)))
    print("".join("\n" + r for r in nemesis.replies if "CALSTORE" in r))
    nemesis.close()
    with open(FIT_FILE, "w") as f:
        json.dump(fit, f, indent=2)
    print("CV fits saved to %s" % FIT_FILE)


def calibrate_outputs(args):
    fit = load_fit()
    patch = [(int(o), cv) for o, cv in (p.split(":") for p in args.patch.split(","))]
    levels = [float(v) for v in args.levels.split(",")]
    nemesis = Nemesis(args.port)
    time.sleep(0.5)
    for output, _ in patch:
        nemesis.send("OUTCAL %d %.4f 0" % (output, OUTPUT_VOLTS))  # uncorrected while measuring
        nemesis.send("DC %d 0" % output)
    time.sleep(0.3)

    def measure(output, cv, level):
        nemesis.send("DC %d %.4f" % (output, level))
        time.sleep(args.settle)
        r = nemesis.average(args.average)[CV_NAMES.index(cv)]
        if not 0.002 < r < 0.998:
            sys.exit("output %d -> %s clipped at level %.2f, check the patch" % (output + 1, cv, level))
        return to_volts(fit, cv, r)

    for output, cv in list(patch):
        measured = [measure(output, cv, l) for l in levels]
        per_unit, offset = np.polyfit(levels, measured, 1)
        if not 4.0 < per_unit < 6.0:
            print("output %d: no usable signal on %s (%.3f V per unit), not calibrated" % (output + 1, cv, per_unit))
            nemesis.send("DC %d 0" % output)
            patch = [p for p in patch if p[0] != output]
            continue
        nemesis.send("OUTCAL %d %.5f %.5f" % (output, per_unit, offset))
        nemesis.send("DC %d 0" % output)
        print("output %d: %.4f V per unit, %+.4f V at 0" % (output + 1, per_unit, offset))
    nemesis.send("CALSTORE")
    time.sleep(0.5)

    # unipolar CV inputs can't read exactly 0 V (bottom of the ADC range), check from 0.2 V
    print("\ncheck with the stored calibration (target %.0f V per unit):" % OUTPUT_VOLTS)
    for output, cv in patch:
        errors = [measure(output, cv, l) - OUTPUT_VOLTS * l for l in (0.04, 0.4, 0.8)]
        nemesis.send("DC %d 0" % output)
        print("  output %d error at 0.2/2/4 V: %s mV" % (output + 1, " / ".join("%+.1f" % (e * 1000) for e in errors)))
    print("".join("\n" + r for r in nemesis.replies if "CALSTORE" in r))
    nemesis.close()


def calibrate_output_dmm(args):
    from hp34401a import HP34401A  # pip install -e ~/GitHub/34401a-calibrator

    output = args.output - 1
    levels = np.linspace(-args.max_level, args.max_level, args.points)
    nemesis = Nemesis(args.port)
    time.sleep(0.5)
    with HP34401A(args.dmm_port) as dmm:
        dmm.configure_dcv(range_=10, nplc=args.nplc)
        dmm.display("NEMESIS OUT%d" % args.output)

        def measure(level):
            nemesis.send("DC %d %.5f" % (output, level))
            mean, sd = dmm.measure(args.readings, settle=args.settle)
            return mean

        # quick check that the meter sees this output before measuring
        nemesis.send("OUTCAL %d %.4f 0" % (output, OUTPUT_VOLTS))  # uncorrected while measuring
        low, high = measure(-0.4), measure(0.4)
        if abs((high - low) - 0.8 * OUTPUT_VOLTS) > 0.5:
            nemesis.send("DC %d 0" % output)
            nemesis.close()
            sys.exit("output %d: meter shows %+.3f V / %+.3f V for -2 V / +2 V, is it connected to output %d?" % (
                args.output, low, high, args.output))

        measured = np.array([measure(l) for l in levels])
        per_unit, offset = np.polyfit(levels, measured, 1)
        linearity_mv = np.max(np.abs(measured - (per_unit * levels + offset))) * 1000
        print("output %d: %.5f V per unit, %+.5f V at 0, linear to %.2f mV over %+.1f..%+.1f V" % (
            args.output, per_unit, offset, linearity_mv, levels[0] * per_unit, levels[-1] * per_unit))

        nemesis.send("OUTCAL %d %.6f %.6f" % (output, per_unit, offset))
        nemesis.send("CALSTORE")
        time.sleep(0.5)
        checks = [(v, measure(v / OUTPUT_VOLTS)) for v in (-4.0, 0.0, 4.0)]
        print("  check: %s" % ",  ".join("%+.1f V -> %+.5f V (%+.2f mV)" % (v, m, (m - v) * 1000) for v, m in checks))
        nemesis.send("DC %d 0" % output)
    stored = [r for r in nemesis.replies if "CALSTORE" in r]
    nemesis.close()
    print("  %s" % (stored[0] if stored else "WARNING: no CALSTORE confirmation from the module"))


def calibrate_inputs_loopback(args):
    patch = [(int(o) - 1, cv) for o, cv in (p.split(":") for p in args.patch.split(","))]
    volts = np.arange(-args.max_volts, args.max_volts + 1e-9, args.step)
    nemesis = Nemesis(args.port)
    time.sleep(0.5)
    for output, _ in patch:
        nemesis.send("DC %d 0" % output)
    fit = load_fit() if os.path.exists(FIT_FILE) else {}

    def read(output, cv, v):
        nemesis.send("DC %d %.5f" % (output, v / OUTPUT_VOLTS))
        time.sleep(args.settle)
        return nemesis.average(args.average)[CV_NAMES.index(cv)]

    for output, cv in patch:
        ch = CV_NAMES.index(cv)
        readings = np.array([read(output, cv, v) for v in volts])
        nemesis.send("DC %d 0" % output)
        valid = (readings > 0.003) & (readings < 0.997)
        if valid.sum() < 6:
            print("%s: only %d unclipped points from output %d, not calibrated" % (cv, valid.sum(), output + 1))
            continue
        slope, intercept = np.polyfit(volts[valid], readings[valid], 1)
        residual_mv = np.max(np.abs(readings[valid] - (slope * volts[valid] + intercept))) / slope * 1000
        zero = 0.5 if cv in BIPOLAR else intercept  # X/Y/Z: the main knob sets the zero
        fit[cv] = {"per_volt": round(slope, 6), "zero": round(intercept, 6)}
        nemesis.send("CVCAL %d %.6f %.6f" % (ch, zero, slope))
        span = volts[valid]
        print("%-5s from output %d: %.6f per volt (design %+.2f%%), 0 V reads %+.5f%s, linear to %.1f mV over %+.1f..%+.1f V" % (
            cv, output + 1, slope, (slope / 0.12 - 1) * 100, intercept,
            " (knob)" if cv in BIPOLAR else "", residual_mv, span[0], span[-1]))

    nemesis.send("CALSTORE")
    time.sleep(0.5)
    print("\ncheck with the stored calibration, A-D (V) and the X/Y/Z gain (difference between +-2 V):")
    for output, cv in patch:
        ch = CV_NAMES.index(cv)
        if cv in BIPOLAR:
            lo, hi = (to_volts(fit, cv, read(output, cv, v)) for v in (-2.0, 2.0))
            print("  %s: +-2 V reads a %.4f V span (%+.2f mV)" % (cv, hi - lo, (hi - lo - 4.0) * 1000))
        else:
            errs = [to_volts(fit, cv, read(output, cv, v)) - v for v in (0.5, 2.5, 4.5)]
            print("  %s error at 0.5/2.5/4.5 V: %s mV" % (cv, " / ".join("%+.2f" % (e * 1000) for e in errs)))
        nemesis.send("DC %d 0" % output)
    print("".join("\n" + r for r in nemesis.replies if "CALSTORE" in r))
    nemesis.close()
    with open(FIT_FILE, "w") as f:
        json.dump(fit, f, indent=2)
    print("CV fits saved to %s" % FIT_FILE)


def audio_inputs(args):
    nemesis = Nemesis(args.port)
    time.sleep(0.5)
    for o in range(8):
        nemesis.send("DC %d 0" % o)
        nemesis.send("SINE %d 0" % o)
    nemesis.send("FREQ %.1f" % args.freq)
    nemesis.send("HPF 1")
    settle = args.settle
    _, noise = nemesis.average_ain(1.0)

    print("1) %g Hz sine on one output at a time, amplitude raised while there's room" % args.freq)
    gains = {}
    for o in range(8):
        peak_v = 0.25
        while True:
            nemesis.send("SINE %d %.5f" % (o, peak_v / OUTPUT_VOLTS))
            time.sleep(settle)
            _, rms = nemesis.average_ain(0.6)
            responding = int(np.argmax(rms))
            in_peak = rms[responding] * np.sqrt(2)  # of full scale
            if in_peak > 0.3 or peak_v >= 4.0:
                break
            # only ever raise the level, aiming at ~half of full scale
            peak_v = min(4.0, peak_v * min(4.0, max(1.25, 0.5 / max(in_peak, 1e-6))))
        nemesis.send("SINE %d 0" % o)
        others = np.delete(rms, responding)
        crosstalk_db = 20 * np.log10(max(others.max(), 1e-9) / rms[responding])
        fs_per_volt = in_peak / peak_v
        gains[o] = (responding, fs_per_volt)
        print("  out %d -> in %d: %.3f V peak gives %.3f of full scale, full scale = %.3f V peak, "
              "next strongest input %.0f dB below" % (o + 1, responding + 1, peak_v, in_peak, 1 / fs_per_volt, crosstalk_db))

    print("\n2) DC with the codec high-pass frozen (inputs 1-6; 7-8 have a fixed 1 Hz high-pass)")
    nemesis.send("HPF 0")
    time.sleep(0.5)
    for o, (inp, fs_per_volt) in gains.items():
        dc_v = min(2.0, 0.4 / fs_per_volt)
        readings = []
        for v in (0.0, dc_v, -dc_v):
            nemesis.send("DC %d %.5f" % (o, v / OUTPUT_VOLTS))
            time.sleep(settle)
            early = nemesis.average_ain(0.5)[0][inp]
            time.sleep(args.hold)
            late = nemesis.average_ain(0.5)[0][inp]
            readings.append((v, early, late))
        nemesis.send("DC %d 0" % o)
        (_, z0, z1), (vp, p0, p1), (vn, n0, n1) = readings
        dc_gain = (p1 - n1) / (2 * dc_v)
        held = abs(p1 - z1) > 0.5 * abs(p0 - z0) if abs(p0 - z0) > 0.01 else False
        print("  in %d: 0 V %+.4f, %+.2f V %+.4f -> %+.4f after %.0f s, %+.2f V %+.4f -> %+.4f  => %s, %s, DC gain %+.3f FS/V (AC %.3f)" % (
            inp + 1, z1, vp, p0, p1, args.hold, vn, n0, n1,
            "DC coupled" if held else "AC coupled",
            "inverted" if dc_gain < 0 else "same polarity", dc_gain, fs_per_volt))

    print("\n3) the same DC step with the high-pass running")
    nemesis.send("HPF 1")
    o, (inp, fs_per_volt) = 0, gains[0]
    dc_v = min(2.0, 0.4 / fs_per_volt)
    nemesis.send("DC %d %.5f" % (o, dc_v / OUTPUT_VOLTS))
    prev_t = 0.0
    for t in (0.5, 1.5, 3.0, 6.0):
        time.sleep(max(0.0, t - prev_t - 0.3))
        prev_t = t
        print("  in %d, %.1f s after a %+.2f V step: %+.4f" % (inp + 1, t, dc_v, nemesis.average_ain(0.3)[0][inp]))
    nemesis.send("DC %d 0" % o)
    nemesis.close()
    print("\nnoise floor before the test (RMS, dBFS): %s" % " ".join("%.0f" % (20 * np.log10(max(n, 1e-9))) for n in noise))


def codec_mode(args):
    """AC gain and DC response of IN 1-6 with CS42448 register 05h set to --reg05."""
    nemesis = Nemesis(args.port)
    time.sleep(0.5)
    for o in range(8):
        nemesis.send("DC %d 0" % o)
        nemesis.send("SINE %d 0" % o)
    nemesis.send("FREQ 1000")
    nemesis.send("REG 0x05 %s" % args.reg05)
    time.sleep(2.0)
    print("register 05h = %s" % args.reg05, flush=True)
    for o in range(6):
        nemesis.send("SINE %d %.5f" % (o, 1.0 / OUTPUT_VOLTS))
        time.sleep(1.5)
        _, rms = nemesis.average_ain(0.5)
        nemesis.send("SINE %d 0" % o)
        ac = rms[o] * np.sqrt(2)
        loudest = int(np.argmax(rms))
        dc = []
        for v in (0.0, 1.0, -1.0):
            nemesis.send("DC %d %.5f" % (o, v / OUTPUT_VOLTS))
            time.sleep(1.5)
            early = nemesis.average_ain(0.4)[0][o]
            time.sleep(2.0)
            dc.append((early, nemesis.average_ain(0.4)[0][o]))
        nemesis.send("DC %d 0" % o)
        dc_gain = (dc[1][1] - dc[2][1]) / 2.0
        print("  in %d: AC %.4f FS/V (loudest input %d)  DC %+.4f FS/V  [0 V %+.4f, +1 V %+.4f -> %+.4f, -1 V %+.4f -> %+.4f]" % (
            o + 1, ac, loudest + 1, dc_gain, dc[0][1], dc[1][0], dc[1][1], dc[2][0], dc[2][1]), flush=True)
    nemesis.close()


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

    p = sub.add_parser("calibrate-inputs", help="calibrate all CV inputs with the ES-10 and store it")
    add_es10_args(p)
    p.add_argument("--min-volts", type=float, default=0.5)
    p.add_argument("--max-volts", type=float, default=7.5)
    p.add_argument("--step", type=float, default=0.5)
    p.add_argument("--settle", type=float, default=0.4)
    p.add_argument("--average", type=float, default=0.6)
    p.add_argument("--port", default="/dev/cu.usbmodem103226001")
    p.set_defaults(func=calibrate_inputs)

    p = sub.add_parser("calibrate-outputs", help="calibrate module outputs through calibrated CV inputs")
    p.add_argument("--patch", default="0:CV_A,1:CV_B,2:CV_C,3:CV_D", help="output index (0 based):CV input pairs")
    p.add_argument("--levels", default="0.04,0.2,0.4,0.6,0.8", help="kept above 0 V for the unipolar CV inputs")
    p.add_argument("--settle", type=float, default=0.4)
    p.add_argument("--average", type=float, default=0.6)
    p.add_argument("--port", default="/dev/cu.usbmodem103226001")
    p.set_defaults(func=calibrate_outputs)

    p = sub.add_parser("calibrate-outputs-dmm", help="calibrate one module output against an HP 34401A")
    p.add_argument("--output", type=int, required=True, help="1..8")
    p.add_argument("--max-level", type=float, default=0.9, help="highest level, 0.9 = 4.5 V")
    p.add_argument("--points", type=int, default=13)
    p.add_argument("--readings", type=int, default=3, help="meter readings averaged per level")
    p.add_argument("--nplc", type=float, default=10)
    p.add_argument("--settle", type=float, default=0.3)
    p.add_argument("--dmm-port", help="meter serial port, default: first USB serial adapter")
    p.add_argument("--port", default="/dev/cu.usbmodem103226001")
    p.set_defaults(func=calibrate_output_dmm)

    p = sub.add_parser("calibrate-inputs-loopback", help="calibrate CV inputs from the calibrated module outputs")
    p.add_argument("--patch", default="1:CV_X,2:CV_Y,3:CV_Z,4:CV_A,5:CV_B,6:CV_C,7:CV_D",
                   help="output (1 based):CV input pairs")
    p.add_argument("--max-volts", type=float, default=4.5)
    p.add_argument("--step", type=float, default=0.5)
    p.add_argument("--settle", type=float, default=0.4)
    p.add_argument("--average", type=float, default=0.6)
    p.add_argument("--port", default="/dev/cu.usbmodem103226001")
    p.set_defaults(func=calibrate_inputs_loopback)

    p = sub.add_parser("audio-inputs", help="mapping, gain, polarity and DC coupling of the audio inputs")
    p.add_argument("--freq", type=float, default=1000)
    p.add_argument("--settle", type=float, default=1.5, help="the input meters average over ~0.3 s")
    p.add_argument("--hold", type=float, default=3.0)
    p.add_argument("--port", default="/dev/cu.usbmodem103226001")
    p.set_defaults(func=audio_inputs)

    p = sub.add_parser("codec-mode", help="AC/DC response of IN 1-6 for a CS42448 register 05h value")
    p.add_argument("--reg05", default="0x1C")
    p.add_argument("--port", default="/dev/cu.usbmodem103226001")
    p.set_defaults(func=codec_mode)

    args = parser.parse_args()
    if args.command == "hold" and abs(args.value) > 1:
        sys.exit("value must be within -1..1")
    args.func(args)


if __name__ == "__main__":
    main()
