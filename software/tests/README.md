# tests

Hardware test firmware and measurement tools. The firmware projects build
like the other projects (`pio run -e rev2 -t upload`). The Python tools need the
environment in `.venv` (not committed):

```sh
python3.12 -m venv .venv && .venv/bin/pip install -r requirements.txt
```

| folder | what it does |
| --- | --- |
| `hwtest` | OLED bars for every input (rev2), serial readout of inputs, buttons and encoder, button LEDs; DC level and sine per output, DC/RMS meters on all 8 audio inputs, codec register access; volt calibration commands (see the top of `src/main.cpp`) |
| `pinscan` | lists which unused Teensy pins change, to find how an unknown control is wired |
| `adcnoise` | measures the pot ADC noise and time per reading for several ADC settings |
| `cvtest` | `cvtest.py`, needs `hwtest` on the module: CV inputs against an ES-10 (`sweep`, `calibrate-inputs`) or the calibrated outputs (`calibrate-inputs-loopback`), outputs in loopback (`dac`, `calibrate-outputs`) or against an HP 34401A (`calibrate-outputs-dmm`), audio inputs (`audio-inputs`, `codec-mode`) |

`cvtest/cv_fit.json` holds the CV input fits of the rev2 test unit, used to
convert readings to volts. The calibration currently stored on that unit:
outputs against the HP 34401A, CV inputs in loopback from those outputs
(2026-11-03).

MIDI tests: `mido` with the "Teensy MIDI" port when the firmware is built
with `-D USB_MIDI_SERIAL` (as `software/firmware` is).

## transport

`transport/run.sh` builds and runs a host test of
`lib/nemesis/src/nemesis_transport_core.h` on the Mac: CV clock tempo, a
transient on the clock input, the tempo after the clock stops, bunched MIDI
clocks. Run it after changing the transport core, before flashing.

## programs

Checks for every program in `firmware/platformio.ini`, in two parts.

`programs/run.sh` (on the Mac, ~30 s): compiles each program with the
firmware's FAUST options and renders it with `program_test.cpp`, the way
the module plays it: transport at 120 BPM, notes and triggers for
`[mono:...]` programs, chords for polyphonic ones, signals on every input.
Phases: defaults, 24 random settings, all minimum, all maximum, recovery
(defaults with silent inputs). It fails on NaN/inf on an output or meter,
an output beyond +-100 (runaway feedback), an audio output beyond +-4 with
the defaults, NaN that stays after the settings are back to normal; it
warns about loud outputs (> 4) with other settings, DC offset, clipping,
silence. A failing phase is searched for its culprit: each setting alone at
its failing value. With a built firmware it also lists the size of every
program's `compute()` against the instruction cache (`codesize.py`).
`./run.sh juno pollen` checks some programs, `SEED=3` other random
settings, `FAUST=...` another compiler. A program whose inputs are all
triggers declares `declare test_inputs "trig";`.

`programs/module.py` (on the module, ~2 min): switches through all
programs and reports CPU, peak, late audio blocks, NaN resets and the heap
after all. `--save` stores the result as `programs/baseline_rev<N>.json`;
without it the run is compared with that baseline (CPU 3 points higher,
peaks above 95 %, late blocks, NaN resets, 16 kB less heap).
