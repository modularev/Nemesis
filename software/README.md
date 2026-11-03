# software

One codebase for both hardware revisions of the Nemesis. Audio programs are
written in [FAUST](https://faust.grame.fr) and compiled for the Teensy 4.1
during the PlatformIO build.

| folder | content |
| --- | --- |
| `lib/nemesis` | hardware library: codec, control inputs, LEDs, buttons, display (rev2) |
| `faust` | FAUST architecture files and `faust_build.py`, which replaces `faust2nemesis` |
| `firmware` | firmware with several programs, chosen with the encoder (rev2) |
| `programs` | FAUST programs shared by the firmware (juno, freqshift) |
| `template` | starting point for your own program |
| `examples/mooSpace` | stereo reverb |
| `examples/polysynth` | oscillator experiments, parked programs are `.bdsp` files |
| `tests` | hardware test firmware and measurement tools, see `tests/README.md` |

## requirements

- [PlatformIO](https://platformio.org) (VS Code extension or `pio` CLI)
- FAUST 2.69 or newer, 2.88 recommended (installed: `brew install faust`).
  2.88 generates much smaller code (about 270 kB less RAM1 for mooSpace),
  which matters once several programs share one firmware. If an older FAUST
  is still installed in `/usr/local`, remove it so the two versions don't
  mix headers. With more than one installed, pick one per project with
  `custom_faust = /path/to/faust`.

No FAUST installation needs to be modified anymore, the architecture files
live in `faust/`.

## build and upload

Every project has an environment per hardware revision. The pin assignments
differ, so always build for the revision you are flashing:

```sh
cd examples/mooSpace
pio run -e rev2 -t upload   # rev2 is the default environment
pio run -e rev1 -t upload
```

Uploads use `upload_protocol = teensy-gui` (the Teensy Loader app opens a
small window): `teensy-cli` failed the first attempt almost every time on
macOS. If a firmware crashes in a loop and the Teensy doesn't show up any
more, press the program button on the Teensy and upload again.

On every build each `src/*.dsp` is compiled to `lib/faust_programs/<name>.h`
and `.cpp` (not committed). Programs are only regenerated when the `.dsp`, the
architecture files or the options change.

## hardware revisions

| | rev1 | rev2 |
| --- | --- | --- |
| controls | 9 knobs, each summed with its CV jack, on Teensy analog pins | CV X/Y/Z: main knob summed with the CV through an attenuverter, CV A–D, all on an LTC1867 SPI ADC; 4 knobs (POT A–D) on Teensy analog pins |
| input names | `CV_X CV_Y CV_Z CV_A`..`CV_F` (`POT_X`..`POT_F` also work) | `CV_X CV_Y CV_Z CV_A CV_B CV_C CV_D`, `POT_A POT_B POT_C POT_D` |
| CV range (0..1) | – | calibrated: A–D 0…8 V, X/Y/Z −4…+4 V (main knob adds an offset); hardware range A–D ~0…8.4 V, X/Y/Z ~±4.2 V, linear to ~5 mV |
| outputs | – | 8, DC coupled, ±5 V; calibrated: 1.0 = 5 V, 0 = 0 V |
| LEDs / buttons | 3 / 3 | 2 push buttons with LEDs, encoder with push switch |
| display | – | 128x32 OLED: program and CPU load, program list on the encoder |
| calibration | knob min/max in EEPROM, hold button 3 at power up | POT A–D: hold both buttons for 2 s (or button 1 at power up); CV inputs and outputs in volts with `tests/cvtest` (±2.5 mV / ±2 mV), both in EEPROM |

Both use the CS42448 codec in TDM mode. Every FAUST channel is a 24 bit
sample carried in two 16 bit TDM slots, up to 8 channels in and out.

On rev2 the pots are converted in the background (ADC interrupt, 32x
hardware averaging) and smoothed adaptively: still when resting, no lag
while turning. `nemesis::encoderPosition()` and `nemesis::encoderPressed()`
read the encoder, `nemesis::rawInput()` an input without calibration and
`nemesis::cvVolts()` a CV input in volts. The output calibration is applied
by the FAUST architecture file to every program.

The rev2 values above were measured on hardware with the tools in `tests/`.

## writing a program

A minimal sketch:

```cpp
#include <nemesis.h>
#include "myProgram.h"   // generated from src/myProgram.dsp

myProgram program;

void setup() { nemesis::begin(program); }
void loop()  { nemesis::update(); }
```

`nemesis::update()` scans the inputs and writes every bound FAUST parameter;
input values 0..1 are scaled to the slider range. A parameter is bound by,
in this order of precedence:

```faust
hslider("mix [rev1:CV_F] [rev2:POT_D]", ...)  // per revision
hslider("smear [hw:CV_X]", ...)               // every revision
hslider("CV_X: Pitch", ...)                   // input name as label prefix
```

`[cv:CV_A]` adds a CV input to the bound knob, like on most Eurorack
modules: `hslider("Filter [rev2:POT_A] [cv:CV_A]", ...)`. On rev2 the CV
jacks A–D have no knob, so bind a parameter to a POT plus its CV rather than
to the CV alone: an unpatched jack reads 0.

Parameters without a binding keep their default value. Buttons and
checkboxes bound to an input are gates (on above 0.5).
`nemesis::printControls()` prints the bindings on the serial monitor.

Per program options go into the `.dsp` file:

```faust
declare options "[midi:on][nvoices:8]";
```

MIDI is read from the MIDI jack (`Serial1`, the rev2 jack is wired as TRS
type B) and, when the Teensy USB type includes MIDI (`-D USB_MIDI_SERIAL`),
from USB. When any program is polyphonic, all programs are compiled with
RTTI (FAUST's `poly-dsp.h` needs `dynamic_cast`; mixing RTTI and non-RTTI
copies of the shared FAUST header code crashed).

A polyphonic program can define an `effect` next to `process`; it runs once
after the voices are mixed (juno's chorus):

```faust
process = voice;   // one voice
effect = chorus;   // after the voice mix
```

The architecture clears a program's DSP state if it ever outputs inf/NaN
(e.g. an unstable filter), so it can't stay silent.

## several programs in one firmware

`firmware/` builds a firmware from a list of programs:

```ini
custom_faust_programs =
    ../programs/juno.dsp
    ../examples/mooSpace/src/mooSpace.dsp
board_build.ldscript = ../lib/nemesis/teensy41_flash.ld
```

`faust_build.py` compiles them (plus any `src/*.dsp`) and generates
`nemesis_programs.h`, so `main.cpp` is just
`nemesis::begin(nemesis_programs)`. A program only takes memory once it is
first selected. On rev2 turning the encoder opens the program list and
pushing it loads the program; the last one is stored after it ran for 3 s
and restored at power up. Hold the encoder while powering up to start the
first program instead (e.g. after a program crashed).

`teensy41_flash.ld` runs the FAUST programs and the C/C++ library code from
flash (through the 32 kB cache) instead of RAM1: mooSpace costs ~6 % more CPU
that way, but RAM1 code dropped from 446 kB to 56 kB for four programs.
The firmware runs at 816 MHz; measured CPU load at 48 kHz: juno 1 % idle and
~10 % per voice, mooSpace ~72 %, freqshift 8 %, superosc 13 %.

Serial commands of the firmware: `PROG <n>`, `CONTROLS`, `STATUS` (MIDI
traffic, voices, NaN resets); it reports the CPU load every 2 s.

## memory

- Large FAUST buffers (1 kB and up) are allocated in the optional PSRAM and
  fall back to RAM when none is fitted. Change the threshold with
  `-D NEMESIS_EXTMEM_THRESHOLD=<bytes>`.
- Audio blocks: `-D NEMESIS_AUDIO_MEMORY=<blocks>` (default 100).
- The block size is set with `-D AUDIO_BLOCK_SAMPLES=<n>` (default 128),
  the sample rate with `-D AUDIO_SAMPLE_RATE_EXACT=48000.0f`.

## compiling a .dsp without PlatformIO

```sh
python3 faust/faust_build.py my.dsp -o out/ -- -double
```
