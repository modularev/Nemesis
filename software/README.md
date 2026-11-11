# software

One codebase for both hardware revisions of the Nemesis. Audio programs are
written in [FAUST](https://faust.grame.fr) and compiled for the Teensy 4.1
during the PlatformIO build.

| folder | content |
| --- | --- |
| `lib/nemesis` | hardware library: codec, control inputs, LEDs, buttons, display (rev2) |
| `faust` | FAUST architecture files and `faust_build.py`, which replaces `faust2nemesis` |
| `programs` | all FAUST programs and their controls, see `programs/README.md`; unfinished ones in `programs/sketches` (not built) |
| `firmware` | firmware with all programs, chosen with the encoder (rev2) |
| `single` | firmware with one program: for rev1, or to try a program alone |
| `template` | starting point for your own program |
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
cd firmware                 # rev2: all programs
pio run -e rev2 -t upload   # rev2 is the default environment
cd ../single                # one program, set in its platformio.ini
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
| CV range (0..1) | – | calibrated: A–D 0…8 V (or 0…5 V, a system setting), X/Y/Z −4…+4 V (main knob adds an offset); hardware range A–D ~0…8.4 V, X/Y/Z ~±4.2 V, linear to ~5 mV |
| outputs | – | 8, DC coupled, ±5 V; calibrated: 1.0 = 5 V, 0 = 0 V |
| LEDs / buttons | 3 / 3 | 2 push buttons with LEDs, encoder with push switch |
| display | – | 128x32 OLED: program and CPU load; encoder turn: program settings, click: program list, hold: system settings; name and value of a parameter while its knob is turned |
| settings | – | text files on the SD card (`/NEMESIS/`): per program the parameters without a knob and the CV A–D routing, system: transport (run, clock, tempo, offset), MIDI channel, knob popup, brightness, CV A–D range (0–5 V or 0–8 V) |
| calibration | knob min/max in EEPROM, hold button 3 at power up | POT A–D: hold both buttons for 2 s (or button 1 at power up); CV inputs and outputs in volts with `tests/cvtest` (±2.5 mV / ±2 mV), both in EEPROM |

Both use the CS42448 codec in TDM mode: 8 channels in and out, each a 32 bit
slot (24 bit audio). The running program reads and writes the TDM DMA
buffers directly (`lib/nemesis/src/nemesis_tdm.h`), without Audio library
blocks: the round trip is two audio blocks plus the codec (24 samples).

The block size is set at runtime, system setting **block size**: auto (the
program's `declare block_size "32";`, else 64), 32, 64 or 128; switching
stops the audio for a moment. The setting isn't stored: every power up
starts with auto. Measured OUT 1 -> IN 1: 88 samples (1.8 ms)
at 32, 152 (3.2 ms) at 64, 280 (5.8 ms) at 128 (16 worked too, 56 samples,
but is left out: a program could use only ~75 % of such a block). The
average CPU load hardly depends on it; the peaks grow with smaller blocks,
and the output must be written before the transmit DMA (a few frames ahead)
reaches it: a program may use about 88 % of a 32 sample block, 94 % at 64. mooSpace and mooSpace2 ask for 64, breaker and growler for 32.
`STATUS` shows the block size and output blocks written too late.

The sample rate is set at runtime too, system setting **sample rate**: auto
(the program's `declare sample_rate "96000";`, else 48 kHz), 48k or 96k
(not stored). MCLK is 49.152 MHz at both rates (MFREQ 100 in register
03h, set at boot): the switch stops the SAI at a frame end, sets the bit
clock divider (MCLK / 4 or / 2) and starts it again in sync, like a block
size change; the codec detects the speed from MCLK / LRCK (1024 / 512) and
isn't powered down (that popped on the DC coupled outputs). The i.MX RT SAI
has no bit clock bypass, so MCLK 24.576 MHz (256 fs at 96 kHz) can't be kept. A program reads its
metadata before its DSP is initialized (`nemesis::applySampleRate()`), so
FAUST's `ma.SR` is the new rate; changing the setting restarts the running
program. A frozen pollen loop saved at the other rate loads too (it plays an
octave up or down). growler,
breaker and pollen ask for 96 kHz (CPU about 2x: growler 53 %, breaker 29 %,
pollen 74 %); mooSpace/mooSpace2 don't fit at 96 kHz. `[nvoices:N]` are the
voices at 48 kHz: at 96 kHz half of them take notes (juno: 12 / 6).

Every program switch, block size and sample rate change fades the outputs
out (5 ms) and in again (10 ms) (`nemesis::outputTarget`, applied by the
architecture file); a rate change also mutes the codec's DACs (register 07h)
for ~30 ms while the codec follows the new clocks.
Test command: `RATE <Hz>` (the setting, 0 auto).

The processor runs at 816 MHz (`board_build.f_cpu`). A program can ask for
less with `declare cpu_mhz "600";`: the library sets the clock before the
program's first block and back to 816 MHz for the next program (no program
uses it now; it was a workaround for a weak 5 V supply on rev2, see
`hardware/rev2/KNOWN_ISSUES.md`). Serial test commands: `CLOCK <MHz>`,
`RAW` (all control inputs unfiltered, for logging).

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
from USB. The programs stay standard FAUST, but FAUST's `MidiUI` and
`poly-dsp.h` aren't used (a program using them could never be freed, and
they cost a lot of RAM: juno ~230 kB). Instead:

- the library maps FAUST's MIDI metadata as FAUST's MidiUI does:
  `[midi:ctrl N]` (and `ctrl N C`), `keyon N`, `keyoff N`, `key N`,
  `keypress N`, `pgm`, `chanpress`, `pitchwheel`/`pitchbend`, `start`,
  `stop`, `clock`. A setting or knob takes the value as its own (shown in
  the menu, saved; a knob catches it like a loaded value);
- `[nvoices:N]` programs run on `faust/faust/dsp/nemesis-poly.h`: N voices
  from the program's `create()` (the memory manager, fast RAM arena),
  `freq`/`key`, `gain`/`vel`/`velocity` and `gate` set per voice, the other
  parameters shared (the first voice's UI, copied to the others every
  block). A voice is computed from its note on until it stayed below
  -66 dB for 10 ms after the note off (juno idle: 2 %); a new note takes a
  free voice, else the oldest releasing, else the oldest playing one. A
  voice that still sounds fades out in 3 ms (gate off), takes the new note
  and fades in over 3 ms; the envelopes go on from their level, as a
  retrigger (a block of gate off alone clicked). CC 120/123 release all
  voices.

`[midi:on]` is kept for FAUST compatibility; the library reads the
metadata of every program.

A polyphonic program can define an `effect` next to `process`; it runs once
after the voices are mixed (juno's chorus):

```faust
process = voice;   // one voice
effect = chorus;   // after the voice mix
```

The architecture clears a program's DSP state if it ever outputs inf/NaN
(e.g. an unstable filter), so it can't stay silent.

## MIDI and transport

The library reads the MIDI jack and USB MIDI in a 100 µs timer interrupt
(both revisions). Note, controller and other channel messages are queued
for the running program (its `[midi:..]`, `[mono:..]` parameters and
voices), filtered by the MIDI channel setting.

The transport is system wide: a position in MIDI clock ticks (24 per
quarter) that runs on the internal tempo or follows the MIDI clock. Clock
ticks are timestamped when they arrive and tracked by a phase locked loop,
so the position moves smoothly and stays within a few samples of the clock
(simulated: 2.6 samples rms at a steady tempo, ~12 on a tempo ramp). MIDI
Start resets it to the downbeat, Stop and Continue stop and resume, Song
Position sets it; when the clock stops without a Stop the position holds,
after 0.3 s the internal tempo takes over (clock `auto`).

Clock `IN 8`: a clock or gate on audio IN 8 (rising above 30 % of its
±2.77 V full scale, back below 15 %; any Eurorack clock works), 1/4, 1/8,
1/16 or 24 pulses per quarter, tracked by the same loop. The edges are
found in the samples, so they are timed to the sample; IN 8 is silent in
the program meanwhile, MIDI clock and Start/Stop are ignored. Pulses after
a pause start the transport at the downbeat, it stops when they stop. The
system settings show a clock's tempo with a `*` (`120.0*`).

System settings: run, clock (auto, internal, MIDI, IN 8), IN 8 pulses,
tempo, clock offset (±50 ms, against latency). FAUST programs use it with
`import("nemesis.lib");`:

```faust
import("nemesis.lib");
sixteenth = nm_grid(6);        // 1 sample impulse on every 1/16 (and at start)
beat      = nm_grid(24);
length    = nm_samples(6);     // samples per 1/16 at the current tempo
```

`nm_position` (ticks, wraps every 16 bars), `nm_running`, `nm_bpm` and
`nm_external` are there too, see `faust/nemesis.lib`. The home screen shows
the clock and tempo of programs that use it.

## program memory

Selecting a program frees the previous one, except MIDI programs (juno,
superosc), which stay loaded (small; FAUST's MIDI objects can't be deleted
safely). Settings and CV routing are kept.

The running program's delay lines up to 32 kB come from a 192 kB arena in
RAM1 (fast, no cache; emptied when the program stops), larger ones from the
PSRAM (mooSpace runs at ~56 % with its diffusers in the arena, ~65 %
without). Placing buffers by FAUST's access counts (`memoryInfo()`) was
tried on the branch `memory-plan`: no gain, mooSpace even 4 % slower, since
delays read in order are cheap from the PSRAM (its cache reads whole
lines). `MEM` shows the free heap; build with `-D NEMESIS_DEBUG_SWITCH` to
trace program switches and buffer placement over serial.

The firmware compiles with `-dlt 16384`: delays longer than 16384 samples
get their exact length instead of the next power of two (mooSpace2: 1141 ->
1059 kB, ~0.25 % CPU for the compare instead of a mask). Prefer `os.oscs` to
`os.osc` for LFOs: `os.osc` reads a 256 kB sine table.

## output levels, clipping and limiting

Outputs are DC coupled, 1.0 = 5 V; beyond the DAC range a sample clips.
Clipping is always detected: the home screen shows CLIP and `STATUS` the
clipped samples per output.

Every program gets the setting "output" (turn the encoder), stored with its
other settings: what happens to its audio outputs above 4.4 V, after the
FAUST code (`lib/nemesis/src/nemesis_output.h`):

| output | |
|---|---|
| clip | nothing, the DAC clips hard |
| soft | transparent to 4.4 V, then bends into 4.9 V (default) |
| tape | gradual saturation over the whole range |
| fold | folds the wave back at 4.9 V (wavefolder) |

CV, gate and V/Oct outputs must stay exact, so only outputs 1/2 are shaped
unless the program declares its audio outputs; it can also change the
default:

```faust
declare audio_outputs "1-6";
declare output "clip";
```

A program can still limit itself: `nm_limiter2(ceiling)` from `nemesis.lib`
(stereo linked, 1 ms lookahead, soft clip; about 0.5 % CPU), whose gain
reduction the home screen shows ("lim -3 dB").

More label metadata: `[volts:1]` gives a CV input in volts (calibrated on
rev2, for V/Oct), `[mono:note]`, `[mono:gate]` and `[mono:velocity]` follow
monophonic MIDI (last note priority, the note stays after release),
`[mono:note1]`…`[mono:note4]` and `[mono:gate1]`…`[mono:gate4]` give the
notes of up to 4 paraphonic voices (a new note takes the lowest free voice,
else the oldest note is stolen), `[mono:rr1]`…`[mono:rr4]` four pitches:
a note played alone sets all four, notes added while keys are held go to
the next one in turn (2, 3, 4, 1 …) and stay, `[mono:sp1]`…`[mono:sp4]` four
pitches spread over the notes played together (one note all four, two
1+2 / 3+4, three 1+2 / 3 / 4, four one each, the lowest note first;
lifting keeps it, a note played alone starts anew),
`[setting:0]` makes a parameter only a CV target (not in the settings
list), with `[volts:1]` a routed CV gives it volts (V/Oct on any CV A–D),
`[scale:log]` maps a knob exponentially, `[unit:Hz]` is shown after values.
`[name:lag]` shows a setting under another name (labels stay unique, they
are what the SD card stores), `[if:sync]` / `[ifnot:sync]` lists it only
while the setting "sync" is on / off (mooSpace2: lag in ms or note values),
`[if:voice=3]` while "voice" is 3. `[edit:v{voice} pitch]` on a knob makes
turning it set the setting "v3 pitch" (with "voice" at 3; strike's POT A-D
edit the selected voice), the popup shows that setting.
`[mono:trig]` counts note-ons (every note a change, also repeated ones);
`[chan:N]` on a mono parameter follows MIDI channel base + N - 1 only (base:
the MIDI channel setting, omni 1), `[note:N]` only that note (strike: a
voice per channel, or notes 36-41).
`[pager:1]` on a setting: button 1 steps it (on release; held while
turning `[edit:]` knobs it moves that setting on every page by the same
amount), the home screen shows the page with `[pagename:v{voice} type]` and
an activity strip of `[activity:N]` bargraphs, the settings list starts with
the page's settings. `[section:name]` adds header rows to the settings list,
`[button:N]` is 1 while button N is held. Rolling (random sounds): an `[action:roll]` setting shows the position in
a history of the last 16 rolls (#0: before rolling); in the list click it,
then every detent right is a new roll, left goes back one. `[action:target]`
(menu: page / all) says what a roll changes, `[action:amount]` how far
(100 % fully random), `[random:0]` keeps a setting out. Button 1 held +
button 2: a new roll of the current page (the program can play it on
`[button:2]`); the header shows "roll #n" for a moment. Edit knobs let go
of a setting that changed elsewhere and catch it again. A roll leaves alone
what is played: settings an edit knob sets (on any page) and settings with a
routed CV A-D that carries a signal (above ~0.2 V). `[title:x]` on a knob
names its popup after the value of setting "x" (assignable knobs). Build options per program: `declare options "[itcm:on]"` runs
compute() from ITCM (for loops bigger than the 32 kB instruction cache),
`[optimize:Os]`, `[faust:...]` extra FAUST options.

Robustness: a hardware watchdog resets the module when loop() stalls for
4 s. Starts that end within 10 s are counted; after two in a row the module
starts program 1 and leaves the SD card alone ("SD off" on the home
screen). Settings files are read at most 32 kB (a longer one is renamed to
`BAD_<name>` and the program keeps its defaults: "SD bad"), and written to
`<name>.NEW` first. A program is remembered for the next power up after it
has run for 10 s. If "SD off" or "SD bad" shows, repair the card on a
computer (e.g. Disk Utility, First Aid) or reformat it (FAT32). On rev2 every knob keeps its last
value with the program's settings and catches it after a start or program
switch (the popup marks the knob's place until it reaches the value).

## several programs in one firmware

`firmware/` builds a firmware from a list of programs:

```ini
custom_faust_programs =
    ../programs/juno.dsp
    ../programs/mooSpace.dsp
board_build.ldscript = ../lib/nemesis/teensy41_flash.ld
```

`faust_build.py` compiles them (plus any `src/*.dsp`) and generates
`nemesis_programs.h`, so `main.cpp` is just
`nemesis::begin(nemesis_programs)`. A program only takes memory once it is
first selected. On rev2 a click on the encoder opens the program list,
turning and clicking again loads a program; the last one is stored after it ran for 3 s
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
- Audio blocks: `-D NEMESIS_AUDIO_MEMORY=<blocks>` (default 4; the programs
  use none, only extra Audio library objects would).
- The block size is set with `-D AUDIO_BLOCK_SAMPLES=<n>` (default 128),
  the sample rate with `-D AUDIO_SAMPLE_RATE_EXACT=48000.0f`.

## compiling a .dsp without PlatformIO

```sh
python3 faust/faust_build.py my.dsp -o out/ -- -double
```
