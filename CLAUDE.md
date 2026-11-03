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
- software/firmware serial commands: `PROG <n>`, `CONTROLS`, `STATUS`
  (MIDI bytes/USB messages, voice notes, NaN resets), CPU report every 2 s.
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

## State (2026-11-03)

- Calibration in the rev2 test unit's EEPROM: outputs vs. HP 34401A (±0.4 mV),
  CV inputs in loopback from them (±1 mV A–D).
- Firmware programs: juno (6 voice MIDI poly, chorus effect), mooSpace,
  freqshift, superosc; 816 MHz, 48 kHz.
- Open: juno stress test with cutoff/resonance/env at max (NaN counter),
  rev1 never tested with the unified code, the 0x44 register mode for DC on
  IN 5/6 untested.
