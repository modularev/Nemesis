# Working on Nemesis

Teensy 4.1 Eurorack DSP module running FAUST programs. Hardware rev1 and
rev2 share one codebase (`software/`, overview in `software/README.md`).
Measured rev2 hardware facts and design issues: `hardware/rev2/KNOWN_ISSUES.md`.

## Tools on this Mac

- PlatformIO CLI: `~/.platformio/penv/bin/pio` (not on PATH).
- FAUST 2.88 from Homebrew (`/opt/homebrew/bin/faust`, first on PATH). An old
  2.69 install remains in `/usr/local`; FAUST searches install dirs before
  `-A`, so never give a repo header the same name as one in
  `/usr/local/include/faust`.
- Python tools: `software/tests/.venv/bin/python` (Python 3.12, pyserial,
  numpy, sounddevice, mido, python-rtmidi, hp34401a). Recreate with
  `software/tests/requirements.txt`. python-rtmidi has no 3.14 wheels.
- No `timeout` command on macOS: use tool timeouts and deadlines in Python.

## Devices

- rev2 test unit: `/dev/cu.usbmodem103226001`, USB MIDI port "Teensy MIDI"
  when built with `-D USB_MIDI_SERIAL` (software/firmware).
- HP 34401A multimeter: `/dev/cu.usbserial-FTEJ7S3Y` (FTDI), RS-232 9600 8N2,
  HP-IB address must not be 31 (talk only). Driver and CLI:
  `~/GitHub/34401a-calibrator` (`hp34401a read -n 5`).
- Fireface UFX outputs 23–30 → ES-10 DC outputs (~11.05 V full scale, only
  handheld-meter accurate; the outputs calibrated against the HP are better).
- The user patches cables and turns knobs on request; say exactly what to
  patch and wait for confirmation.

## Simulator

- Nemesis Sim (AU/VST3 plugins that run the .dsp files in Ableton) lives in
  its own repo, `~/GitHub/nemesis-simulator`. The user asked not to work on
  it until they ask: don't change, build or sync it with library changes.

## Build, upload, debug

- `pio run -e rev2 -t upload` in a project dir; all projects use
  `upload_protocol = teensy-gui` (teensy-cli failed the first attempt almost
  always). After an upload the serial port appears after ~5–10 s.
- If the Teensy is gone from USB (crash loop), ask the user to press the
  program button, then upload once (bootloader shows as idProduct 1144 in
  `ioreg -p IOUSB -l`).
- Firmwares print `CrashReport` at start; decode the address with
  `arm-none-eabi-addr2line` from `~/.platformio/packages/toolchain-gccarmnoneeabi-teensy/bin`.
- Use short tool timeouts (upload ~60 s) and keep long hardware tests in
  short steps with progress output.
- software/firmware serial commands: `PROG <n>`, `CONTROLS`, `MEM` (PSRAM, 16 MB on the rev2 unit),
  `GET <path>` (parameter or meter, e.g. `/breaker/slice`), `STATUS` (MIDI bytes/USB messages,
  transport state, voice notes, NaN resets), CPU report every 2 s.
- The transport PLL (`lib/nemesis/src/nemesis_transport_core.h`) is plain C++: test changes on
  the Mac with a host harness before flashing.
- tests/hwtest serial commands: see the top of `software/tests/hwtest/src/main.cpp`
  (DC/SINE/FREQ per output, AIN meters, REG codec write, CVCAL/OUTCAL/CALSTORE).

## Things that bit us

- FAUST `attach(0)` means `attach(_, 0)` (last argument first): use
  `(0, _) : attach` to keep a meter without outputting it.
- With `-mem` the generated `init()` is empty: call `classInit()` and
  `instanceInit()`.
- FAUST's poly DSP registers itself for MIDI when `buildUserInterface(MidiUI)`
  runs; an extra `addMidiIn()` doubles every note.
- RTTI must be all or nothing across the generated programs (shared inline
  FAUST header code); faust_build.py enables it for all when one is poly.
- `ve.moog_vcf` is only valid below SR/6.3; it blew up juno. `ve.moogLadder`
  is stable to 20 kHz.
- `AudioMemory()` needs a compile time constant (`NEMESIS_AUDIO_MEMORY`).
- The flash linker script must discard `.ARM.exidx` of the code it moves to
  flash (31 bit offsets can't reach from ITCM).
- rev2 CV jacks A–D have no knobs: bind parameters to a POT plus `[cv:CV_x]`,
  an unpatched jack reads 0.
- CPU: Mac benchmarks of FAUST code don't predict the Teensy (PSRAM reads,
  memory placement): measure on the module (PROG <n>, CPU report). A program
  that needs > 100 % starves loop(): no serial output at all. FAUST evaluates
  pow/tan/log of smoothed knobs on every sample: compute from the raw slider
  and smooth the result. Gliding and crossfading heads side by side on every
  mooSpace delay overloaded; one kind of head with motion blending lengths
  costs ~1.7 % per delay line.
- Program switching (stop/start): FAUST's poly DSP registers with the MidiUI
  (delete DSP, then MidiUI, then the handler); deleting a MidiUI while another
  program's MidiUI exists hangs, so MIDI programs are never stopped. start()
  and stop() run with AudioNoInterrupts (FAUST's global MIDI tables) and
  update() waits for fReady.

## State (2026-11-03)

- Calibration in the rev2 test unit's EEPROM: outputs vs. HP 34401A (±0.4 mV),
  CV inputs in loopback from them (±1 mV A–D).
- Firmware programs: juno (6 voice MIDI poly, chorus effect), mooSpace,
  breaker (beat slice repeat/reverse, crush, comb, drive, drum triggers on
  outs 3–8 from a pattern map; on the transport),
  freqshift, superosc, growler (mono FM bass, 14 %); 816 MHz, 48 kHz.
- mooSpace2 (program 3): v1 algorithm with wider controls, measured 67 %
  (max 72 %) on the module; motion only on the tank delays (CPU). mooSpace v1
  56 % since the RAM arena. freqshift 13 %, superosc 25 %, breaker 14 %.
- Output setting (clip/soft/tape/fold) on every program's audio outputs
  (nemesis_output.h, `declare audio_outputs`/`declare output`); breaker
  defaults to clip, freqshift shapes 1-6. superosc's oscillator sum scales
  with root+fourth+fifth (it drove its limiter to -8 dB).
- Open: freqshift (rewritten), superosc (drone, MIDI/V-Oct, limiter), CLIP and
  limiter display only simulated, not run on the module; check CPU of both.
- Open: MIDI input in the 100 us timer interrupt and the transport PLL are
  only simulated, not yet run on the module (check STATUS, juno notes over
  USB and the jack, breaker on a DAW clock, CPU load of the poll timer).
- Direct TDM, measured 2026-11-06: programs read/write the TDM DMA
  buffers directly (nemesis_tdm.cpp), loopback OUT 1 -> IN 1 281 samples
  (5.85 ms; main 537, the codec adds 25), level identical, "written late" 0
  of 21617 blocks (mooSpace/mooSpace2 at ~60 %). CPU: mooSpace2 58 % (main
  62), breaker 12, growler 14, rest unchanged. The memory plan from
  memoryInfo() (branch memory-plan, not on main) gains nothing (mooSpace v1
  60 % with it, 56 % without): access counts don't predict cost, PSRAM
  reads in order are cheap. Not yet checked: other channels' mapping,
  calibration with the HP, hwtest, rev1.
- Idea, parked: 96 kHz. CS42448 TDM supports double speed (MCLK 512x = 49.2 MHz,
  register 0x03 MFREQ needs 1xx), but the datasheet warns ADC_SDOUT may miss
  timing in TDM double speed: measure the inputs first (a rev2_96k build).
  CPU doubles (mooSpace won't fit). Block size 128 is compile time and the max.
- Open: juno stress test with cutoff/resonance/env at max (NaN counter),
  rev1 never tested with the unified code, the 0x44 register mode for DC on
  IN 5/6 untested.
