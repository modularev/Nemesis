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
